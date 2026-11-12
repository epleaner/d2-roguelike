"""Fetch the published Roguelike release without executing downloaded code."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
import json
import re
import stat
from urllib.parse import urlparse
from urllib.request import HTTPRedirectHandler, Request, build_opener
import zipfile


_REPOSITORY = "epleaner/d2-roguelike"
_LATEST = f"https://api.github.com/repos/{_REPOSITORY}/releases/latest"
_ASSET_NAME = "roguelike-mod.zip"
_ALLOWED_HOSTS = frozenset({"api.github.com", "github.com", "release-assets.githubusercontent.com",
                            "objects.githubusercontent.com", "github-releases.githubusercontent.com"})
_TAG = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_MAX_API = 1024 * 1024
_MAX_PACKAGE = 64 * 1024 * 1024
_MAX_PLAN = 8 * 1024 * 1024
_MAX_DLL = 32 * 1024 * 1024
_HEADERS = {"Accept": "application/vnd.github+json", "User-Agent": "D2-Roguelike-Launcher",
            "X-GitHub-Api-Version": "2022-11-28"}


@dataclass(frozen=True)
class Release:
    version: str
    url: str
    sha256: str
    size: int


@dataclass(frozen=True)
class Package:
    version: str
    revision: str
    plan: dict
    dll: bytes


class _TrustedRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        _check_url(newurl)
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def _check_url(url: str) -> None:
    parsed = urlparse(url)
    if (parsed.scheme != "https" or parsed.hostname not in _ALLOWED_HOSTS or
            parsed.username is not None or parsed.password is not None or
            parsed.port not in (None, 443)):
        raise ValueError("Release download redirected to an untrusted address")


def _read_bounded(url: str, limit: int) -> bytes:
    _check_url(url)
    request = Request(url, headers=_HEADERS)
    with build_opener(_TrustedRedirects()).open(request, timeout=30) as response:
        _check_url(response.geturl())
        length = response.headers.get("Content-Length")
        if length is not None and (not length.isdecimal() or int(length) > limit):
            raise ValueError("Release response is too large")
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Release response is too large")
    return data


def latest() -> Release:
    """Resolve the one published mod package from the fixed public repository."""
    payload = json.loads(_read_bounded(_LATEST, _MAX_API))
    if not isinstance(payload, dict) or payload.get("draft") is not False or payload.get("prerelease") is not False:
        raise ValueError("No stable Roguelike release is available")
    version = payload.get("tag_name")
    if not isinstance(version, str) or not _TAG.fullmatch(version):
        raise ValueError("Release version is invalid")
    assets = payload.get("assets")
    if not isinstance(assets, list):
        raise ValueError("Release has no asset list")
    matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == _ASSET_NAME]
    if len(matches) != 1 or matches[0].get("state") != "uploaded":
        raise ValueError("Release does not contain one complete mod package")
    asset = matches[0]
    digest = asset.get("digest")
    size = asset.get("size")
    url = asset.get("browser_download_url")
    expected_url = f"https://github.com/{_REPOSITORY}/releases/download/{version}/{_ASSET_NAME}"
    if (not isinstance(digest, str) or not digest.startswith("sha256:") or
            not _SHA256.fullmatch(digest[7:]) or type(size) is not int or
            not 0 < size <= _MAX_PACKAGE or url != expected_url):
        raise ValueError("Release package identity is invalid")
    _check_url(url)
    return Release(version, url, digest[7:], size)


def _file(archive: zipfile.ZipFile, name: str, limit: int) -> bytes:
    info = archive.getinfo(name)
    mode = info.external_attr >> 16
    if (info.is_dir() or stat.S_ISLNK(mode) or info.flag_bits & 1 or
            info.file_size > limit or info.file_size < 1):
        raise ValueError("Release contains an invalid package member")
    with archive.open(info) as stream:
        data = stream.read(limit + 1)
    if len(data) != info.file_size or len(data) > limit:
        raise ValueError("Release package member has an invalid size")
    return data


def download(release: Release) -> Package:
    """Download, hash and parse the exact package announced by GitHub."""
    raw = _read_bounded(release.url, min(release.size, _MAX_PACKAGE))
    if len(raw) != release.size or hashlib.sha256(raw).hexdigest() != release.sha256:
        raise ValueError("Downloaded package does not match GitHub's release digest")
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        if sorted(archive.namelist()) != ["BH.dll", "data-plan.json", "manifest.json"]:
            raise ValueError("Release package has unexpected or duplicate members")
        manifest = json.loads(_file(archive, "manifest.json", 65536))
        plan_bytes = _file(archive, "data-plan.json", _MAX_PLAN)
        dll = _file(archive, "BH.dll", _MAX_DLL)
    if (not isinstance(manifest, dict) or set(manifest) != {"schema_version", "version", "revision", "files"}
            or type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1
            or manifest["version"] != release.version
            or not isinstance(manifest["revision"], str)
            or not re.fullmatch(r"mod_[0-9a-f]{64}", manifest["revision"])):
        raise ValueError("Release manifest is invalid")
    expected = manifest["files"]
    if not isinstance(expected, dict) or set(expected) != {"data-plan.json", "BH.dll"}:
        raise ValueError("Release manifest has an invalid file inventory")
    for name, data in (("data-plan.json", plan_bytes), ("BH.dll", dll)):
        record = expected[name]
        if (not isinstance(record, dict) or set(record) != {"sha256", "bytes"} or
                type(record["bytes"]) is not int or record["bytes"] != len(data) or
                record["sha256"] != hashlib.sha256(data).hexdigest()):
            raise ValueError("Release file failed its manifest check: " + name)
    plan = json.loads(plan_bytes)
    if not isinstance(plan, dict):
        raise ValueError("Release data plan is invalid")
    return Package(release.version, manifest["revision"], plan, dll)
