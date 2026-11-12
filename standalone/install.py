"""Install a verified Roguelike release into the player-selected PD2 folder."""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import uuid

from standalone import mod_data_plan
from standalone.releases import Package


_IMAGES = {
    "Game.exe": "74fe9c092a521f7710392548c82f81544e531107db2358617e83818874db40a2",
    "D2Client.dll": "dd8bc6025de921216a97c17f97cd1a50fbb85926e838ec60e13451448836d906",
    "D2Common.dll": "59fa5928522f566f2bf99675571206ad70df889c89d3d07fa87edf5083e06e10",
    "D2Net.dll": "99af2f04a87fdbd91810be963df9c365cef529ae22e6a345193ba5018e9910c9",
    "D2Win.dll": "ff5cbb956f1a419e0e76fc2df3be21065bbba894770b8de7cce8bc55ec453736",
    "D2Launch.dll": "99cabc4751c95d035b5e77151b3605b809884fec2c10edf37d4c4e4e231e8479",
    "D2Gfx.dll": "50284647a28e8839028e8acb021a6ebe48c006e431139969ccd0fe126e6e8811",
    "D2Lang.dll": "206386a81f4046c693f2aca60b9ee19fffd05b1157bbb60992c0d4caad9b67ec",
    "ProjectDiablo.dll": "538a77b7ccef3d5334e56c4e9e57a4d8fc69a1e27c46beb694c0dedfcfbf9cb3",
    "D2Game.dll": "24b293b86f95bea0228826c502ea04737ba21b91ca9832e52aac4b11fef14f53",
}
_OFFICIAL_BH = "cba04ff9e6f944942d69e4b610eeb0a8434bc780d312970ea922877fd134ac74"
_METADATA = ".roguelike-launcher"
_OWNER = "owner.json"
_RECEIPT = "installed.json"
_PENDING = "pending.json"
_REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
_ENGINE_AUXILIARY_BINS = frozenset({
    "global/excel/itemscode.bin", "global/excel/leveldefs.bin",
    "global/excel/misscode.bin", "global/excel/skilldesccode.bin",
    "global/excel/skillscode.bin",
})
_TARGETS = (
    ("ProjectD2/BH.dll", "new-BH.dll", False),
    ("ProjectD2/ProjectDiablo.json", "new-config.json", False),
    ("ProjectD2/AppData/launcherSettings.json", "new-settings.json", False),
    ("ProjectD2/data", "rendered/data", True),
    (f"{_METADATA}/{_RECEIPT}", "new-receipt.json", False),
)


def _hash(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _is_link(path: Path) -> bool:
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & _REPARSE)


def _no_link_ancestors(path: Path) -> None:
    for item in (path, *path.parents):
        if (item.exists() or item.is_symlink()) and _is_link(item):
            raise ValueError(f"Linked or redirected paths are not supported: {item}")


def _walk(root: Path):
    def fail(error):
        raise error
    for directory, names, files in os.walk(root, followlinks=False, onerror=fail):
        for name in names + files:
            yield Path(directory) / name


def _no_links_in_copy(root: Path) -> None:
    for item in _walk(root):
        if _is_link(item):
            raise ValueError(f"The selected game folder contains a link or junction: {item}")
        if item.is_file() and item.stat().st_nlink != 1:
            raise ValueError(f"The selected game folder contains a hard-linked file: {item}")


def _load_json(path: Path) -> dict:
    _no_link_ancestors(path)
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object: {path.name}")
    return value


def _configured_path(config: dict, key: str) -> str:
    try:
        value = config["classic_game_settings"]["other"][key]
    except (KeyError, TypeError) as error:
        raise ValueError("PD2 configuration does not contain the expected game paths") from error
    if not isinstance(value, str):
        raise ValueError("PD2 configuration contains an invalid game path")
    return value


def _same_path(left: str | Path, right: str | Path) -> bool:
    return os.path.normcase(os.path.normpath(str(left))) == os.path.normcase(os.path.normpath(str(right)))


def _verify_native_path(copy: Path) -> None:
    # The current native route snapshot uses ANSI paths and MAX_PATH, including
    # a 15-character hero name and its temporary-file suffix.
    longest = str(copy / "ProjectD2" / ("roguelike-route-" + "A" * 15 + ".json.tmp"))
    if not longest.isascii() or len(longest) >= 260:
        raise ValueError("Use a shorter installation path containing only ASCII characters")


def _selected_root(folder: Path) -> Path:
    folder = Path(folder).absolute()
    _no_link_ancestors(folder)
    if not folder.is_dir():
        raise ValueError("Choose an existing PD2 installation folder")
    folder = folder.resolve(strict=True)
    _verify_native_path(folder)
    return folder


def _verify_images(project: Path) -> None:
    for name, expected in _IMAGES.items():
        path = project / name
        _no_link_ancestors(path)
        if not path.is_file() or _hash(path) != expected:
            raise ValueError(f"This is not the supported PD2 S13 patch #2 installation: {name}")


def _game_process_paths() -> list[str]:
    if os.name != "nt":
        raise RuntimeError("The Roguelike launcher requires Windows")
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    command = ("$ErrorActionPreference='Stop'; "
               "$paths=@(Get-CimInstance Win32_Process -Filter \"Name='Game.exe'\" | "
               "ForEach-Object { if (-not $_.ExecutablePath) { throw 'Cannot identify a Game.exe process' }; "
               "[string]$_.ExecutablePath }); ConvertTo-Json -InputObject $paths -Compress")
    result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, text=True, encoding="utf-8-sig", check=True,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=30)
    value = json.loads(result.stdout)
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise RuntimeError("Could not identify all running Game.exe processes")
    return value


def _require_stopped(project: Path) -> None:
    executable = project / "Game.exe"
    if any(_same_path(path, executable) for path in _game_process_paths()):
        raise ValueError("Close the Roguelike game normally before installing or updating")


def _check_data(root: Path, records: list[dict]) -> None:
    expected = {}
    for record in records:
        path = record["path"]
        if not isinstance(path, str) or not path.startswith("data/"):
            raise ValueError("Installed data inventory is invalid")
        relative = path[5:]
        target = root / relative
        if target.resolve(strict=False).is_relative_to(root.resolve(strict=True)) is False:
            raise ValueError("Installed data inventory escapes the selected game folder")
        if relative.casefold() in expected:
            raise ValueError("Installed data inventory contains duplicate paths")
        expected[relative.casefold()] = record
    actual = {}
    for path in _walk(root):
        if _is_link(path):
            raise ValueError("Installed mod data contains a link or junction")
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative.casefold() in actual:
            raise ValueError("Installed mod data has case-colliding files")
        actual[relative.casefold()] = path
    for relative, record in expected.items():
        path = actual.pop(relative, None)
        if path is None or path.stat().st_size != record["bytes"] or _hash(path) != record["sha256"]:
            raise ValueError("Installed mod data was changed outside the launcher")
    compiled = {name[:-4] + ".bin" for name in expected
                if name.startswith("global/excel/") and name.endswith(".txt")}
    compiled.update(_ENGINE_AUXILIARY_BINS)
    if any(name not in compiled for name in actual):
        raise ValueError("Installed mod data contains unowned files; preserve and inspect them before updating")


def _check_owner(meta: Path, folder: Path) -> None:
    owner = _load_json(meta / _OWNER)
    if (owner.get("schema_version") != 1 or not isinstance(owner.get("copy"), str)
            or not _same_path(owner["copy"], folder)):
        raise ValueError("Launcher metadata belongs to a different game folder")


def inspect(folder: Path) -> dict:
    """Validate the selected game folder and return its installed state."""
    folder = _selected_root(folder)
    _no_links_in_copy(folder)
    project = folder / "ProjectD2"
    _verify_images(project)
    config = _load_json(project / "ProjectDiablo.json")
    meta = folder / _METADATA
    receipt = meta / _RECEIPT
    if (meta / _PENDING).exists():
        raise RuntimeError("A previous update needs recovery before continuing")
    if meta.exists():
        _check_owner(meta, folder)
    if receipt.is_file():
        state = _load_json(receipt)
        if (state.get("schema_version") != 1 or not isinstance(state.get("copy"), str)
                or not _same_path(state["copy"], folder)):
            raise ValueError("Installed launcher state does not match this game folder")
        if not _same_path(_configured_path(config, "installpath"), folder):
            raise ValueError("Selected PD2 install path changed; inspect before continuing")
        if not _same_path(_configured_path(config, "save_path"), folder / "Save"):
            raise ValueError("The selected PD2 save path is not isolated; inspect before continuing")
        if not (project / "BH.dll").is_file() or _hash(project / "BH.dll") != state.get("dll_sha256"):
            raise ValueError("Installed native mod was changed outside the launcher")
        _check_data(project / "data", state["files"])
        return state
    _configured_path(config, "installpath")
    _configured_path(config, "save_path")
    if (project / "data").exists() or (project / "data").is_symlink():
        raise ValueError("The selected game already has loose mod data; it will not be overwritten")
    if not (project / "BH.dll").is_file() or _hash(project / "BH.dll") != _OFFICIAL_BH:
        raise ValueError("The selected game's original BH.dll is not the supported version")
    return {"schema_version": 1, "installed": False, "copy": str(folder)}


def _atomic_json(path: Path, value: dict) -> None:
    data = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8") + b"\n"
    fd, temporary = tempfile.mkstemp(prefix=".launcher-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _tree_hash(path: Path) -> str | None:
    if not path.exists():
        return None
    if not path.is_dir():
        raise ValueError("Expected an owned data directory")
    entries = []
    for item in sorted(_walk(path)):
        if _is_link(item):
            raise ValueError("Transaction directory contains a link or junction")
        if item.is_file():
            entries.append((item.relative_to(path).as_posix(), item.stat().st_size, _hash(item)))
    return hashlib.sha256(json.dumps(entries, separators=(",", ":")).encode()).hexdigest()


def _identity(path: Path, directory: bool) -> str | None:
    if directory:
        return _tree_hash(path)
    if not path.exists():
        return None
    if not path.is_file() or _is_link(path):
        raise ValueError("Transaction target is not a regular file")
    return _hash(path)


def _remove_transaction(transaction: Path) -> None:
    def clear_readonly(function, path, error):
        if os.name != "nt" or not isinstance(error[1], PermissionError):
            raise error[1].with_traceback(error[2])
        os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
        function(path)

    shutil.rmtree(transaction, onerror=clear_readonly)


def _restore(copy: Path, transaction: Path, entries: list[dict]) -> None:
    # Validate all targets and required backups before restoring any component.
    for entry in entries:
        target, backup = copy / entry["target"], transaction / entry["backup"]
        _no_link_ancestors(target)
        _no_link_ancestors(backup)
        current = _identity(target, entry["directory"])
        if current not in (entry["before"], entry["after"], None):
            raise RuntimeError("A pending transaction target changed; inspect it before recovery")
        if (current != entry["before"] and entry["before"] is not None and
                _identity(backup, entry["directory"]) != entry["before"]):
            raise RuntimeError("A pending transaction backup changed; inspect it before recovery")
    for index, entry in reversed(list(enumerate(entries))):
        target, backup = copy / entry["target"], transaction / entry["backup"]
        current = _identity(target, entry["directory"])
        if current == entry["before"]:
            continue
        _require_stopped(copy / "ProjectD2")
        if current is not None:
            os.replace(target, transaction / f"discard-{index}-{uuid.uuid4().hex}")
        if entry["before"] is not None:
            os.replace(backup, target)
    for entry in entries:
        if _identity(copy / entry["target"], entry["directory"]) != entry["before"]:
            raise RuntimeError("Pending update recovery was not confirmed")


@contextmanager
def _lock(meta: Path):
    path = meta / ".lock"
    _no_link_ancestors(path)
    with path.open("a+b") as stream:
        opened = os.fstat(stream.fileno())
        _no_link_ancestors(path)
        if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or
                not os.path.samestat(opened, path.stat(follow_symlinks=False))):
            raise ValueError("The launcher lock is linked or changed; inspect the copied installation")
        if os.name == "nt":
            import msvcrt
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise RuntimeError("Another Roguelike launcher is updating this folder") from error
        else:
            import fcntl
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise RuntimeError("Another Roguelike launcher is updating this folder") from error
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def _journal(meta: Path) -> tuple[Path, list[dict]]:
    pending = _load_json(meta / _PENDING)
    if (set(pending) != {"schema_version", "transaction", "entries"} or
            type(pending["schema_version"]) is not int or pending["schema_version"] != 1 or
            not isinstance(pending["transaction"], str) or
            not re.fullmatch(r"transaction-[0-9a-f]{32}", pending["transaction"]) or
            not isinstance(pending["entries"], list) or len(pending["entries"]) != len(_TARGETS)):
        raise RuntimeError("Pending update journal is invalid")
    transaction = meta / pending["transaction"]
    _no_link_ancestors(transaction)
    if not transaction.is_dir():
        raise RuntimeError("Pending update recovery directory is missing")
    for index, (entry, (target, staged, directory)) in enumerate(zip(pending["entries"], _TARGETS)):
        if (not isinstance(entry, dict) or
                set(entry) != {"target", "staged", "backup", "directory", "before", "after"} or
                entry["target"] != target or entry["staged"] != staged or
                entry["backup"] != f"backup-{index}" or entry["directory"] is not directory or
                any(value is not None and
                    (not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value))
                    for value in (entry["before"], entry["after"])) or
                entry["after"] is None):
            raise RuntimeError("Pending update journal contains unsafe targets")
    return transaction, pending["entries"]


def recover(folder: Path) -> bool:
    """Roll back a recognized interrupted update without changing saves."""
    folder = _selected_root(folder)
    meta, project = folder / _METADATA, folder / "ProjectD2"
    if not (meta / _PENDING).exists():
        return False
    _no_links_in_copy(folder)
    _verify_images(project)
    _check_owner(meta, folder)
    with _lock(meta):
        _require_stopped(project)
        transaction, entries = _journal(meta)
        _restore(folder, transaction, entries)
        (meta / _PENDING).unlink()
        _remove_transaction(transaction)
    return True


def _verify_x86_dll(data: bytes) -> None:
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Release native module is not a Windows DLL")
    offset = int.from_bytes(data[60:64], "little")
    if (offset + 24 > len(data) or data[offset:offset + 4] != b"PE\0\0" or
            data[offset + 4:offset + 6] != b"L\x01" or
            not int.from_bytes(data[offset + 22:offset + 24], "little") & 0x2000):
        raise ValueError("Release native module must be a Windows x86 DLL")


def _prepared_configs(project: Path, copy: Path) -> tuple[dict, dict]:
    config = _load_json(project / "ProjectDiablo.json")
    _configured_path(config, "installpath")
    _configured_path(config, "save_path")
    config["classic_game_settings"]["other"]["installpath"] = str(copy) + os.sep
    config["classic_game_settings"]["other"]["save_path"] = str(copy / "Save") + os.sep
    settings = _load_json(project / "AppData/launcherSettings.json")
    args = settings.get("LauncherArgs")
    if not isinstance(args, dict):
        raise ValueError("PD2 launcher settings are incompatible")
    args["disableAutoUpdate"] = True
    args["skiptobnet"] = False
    return config, settings


def _stage_release(folder: Path, transaction: Path, package: Package) -> tuple[dict, list[dict]]:
    project = folder / "ProjectD2"
    config, settings = _prepared_configs(project, folder)
    result = mod_data_plan.render_plan(package.plan, project / "pd2data.mpq",
                                       folder / "d2data.mpq", transaction / "rendered")
    if result["revision"] != package.revision:
        raise ValueError("Rendered revision differs from the downloaded release")
    (transaction / "new-BH.dll").write_bytes(package.dll)
    _atomic_json(transaction / "new-config.json", config)
    _atomic_json(transaction / "new-settings.json", settings)
    state = {"schema_version": 1, "installed": True, "copy": str(folder),
             "version": package.version, "revision": package.revision,
             "dll_sha256": hashlib.sha256(package.dll).hexdigest(), "files": result["files"]}
    _atomic_json(transaction / "new-receipt.json", state)
    entries = []
    for index, (name, staged, directory) in enumerate(_TARGETS):
        target, backup = folder / name, transaction / f"backup-{index}"
        previous = _identity(target, directory)
        if previous is not None:
            if directory:
                shutil.copytree(target, backup)
            else:
                shutil.copy2(target, backup)
            if _identity(backup, directory) != previous:
                raise RuntimeError("Update backup did not match its source")
        entries.append({"target": name, "staged": staged, "backup": f"backup-{index}",
                        "directory": directory, "before": previous,
                        "after": _identity(transaction / staged, directory)})
    return state, entries


def _commit_release(copy: Path, transaction: Path, entries: list[dict]) -> None:
    pending, project = copy / _METADATA / _PENDING, copy / "ProjectD2"
    _atomic_json(pending, {"schema_version": 1, "transaction": transaction.name, "entries": entries})
    try:
        for index, entry in enumerate(entries):
            _require_stopped(project)
            target = copy / entry["target"]
            if _identity(target, entry["directory"]) != entry["before"]:
                raise RuntimeError("Game folder changed during the update")
            if target.exists():
                os.replace(target, transaction / f"old-{index}")
            os.replace(transaction / entry["staged"], target)
    except BaseException:
        # A concurrently started game makes rollback unsafe too. Keep the
        # journal and backups for explicit recovery after normal game exit.
        _require_stopped(project)
        _restore(copy, transaction, entries)
        pending.unlink()
        raise
    pending.unlink()


def install(folder: Path, package: Package) -> dict:
    """Render and transact a release into the selected folder, preserving saves."""
    folder = _selected_root(folder)
    before = inspect(folder)
    project, meta = folder / "ProjectD2", folder / _METADATA
    _require_stopped(project)
    if package.plan.get("revision", {}).get("revision") != package.revision:
        raise ValueError("Package revision does not match its data plan")
    _verify_x86_dll(package.dll)
    if (before.get("installed") and before.get("version") == package.version and
            before.get("revision") == package.revision and
            before.get("dll_sha256") == hashlib.sha256(package.dll).hexdigest()):
        return before
    created_meta = not meta.exists()
    if created_meta:
        meta.mkdir()
        try:
            _atomic_json(meta / _OWNER, {"schema_version": 1, "copy": str(folder)})
        except BaseException:
            if not any(meta.iterdir()):
                meta.rmdir()
            raise
    try:
        with _lock(meta):
            if inspect(folder) != before:
                raise RuntimeError("Game folder changed before the update")
            if created_meta and {entry.name for entry in meta.iterdir()} != {".lock", _OWNER}:
                raise RuntimeError("New launcher metadata contains unexpected files")
            transaction = meta / ("transaction-" + uuid.uuid4().hex)
            transaction.mkdir()
            try:
                state, entries = _stage_release(folder, transaction, package)
                _require_stopped(project)
                if inspect(folder) != before:
                    raise RuntimeError("Game folder changed while the update was prepared")
                (folder / "Save").mkdir(exist_ok=True)
                _commit_release(folder, transaction, entries)
            except BaseException:
                if not (meta / _PENDING).exists() and transaction.exists():
                    _remove_transaction(transaction)
                raise
            _remove_transaction(transaction)
            return state
    finally:
        if created_meta and meta.exists() and {entry.name for entry in meta.iterdir()} == {".lock", _OWNER}:
            (meta / ".lock").unlink()
            (meta / _OWNER).unlink()
            meta.rmdir()


def _prepare_resolution_config(project: Path) -> None:
    # PD2's resolution module adds missing defaults on startup. A copied
    # read-only config otherwise makes the game stop with an error dialog.
    config = project / "SGD2FreeResolution.json"
    if not config.exists():
        return
    if not config.is_file():
        raise ValueError("The copied SGD2FreeResolution.json is not a regular file")
    mode = config.stat().st_mode
    if not mode & stat.S_IWRITE:
        try:
            config.chmod(mode | stat.S_IWRITE)
        except OSError as error:
            raise ValueError("Cannot make the copied SGD2FreeResolution.json writable") from error


def launch(folder: Path) -> int:
    """Start the installed game; the player chooses Single Player."""
    folder = _selected_root(folder)
    if not (folder / _METADATA / _RECEIPT).is_file():
        raise ValueError("Install Roguelike before launching")
    with _lock(folder / _METADATA):
        state = inspect(folder)
        if not state.get("installed"):
            raise ValueError("Install Roguelike before launching")
        project = folder / "ProjectD2"
        _require_stopped(project)
        if not (folder / "Save").is_dir():
            raise ValueError("The isolated Roguelike Save directory is missing")
        _prepare_resolution_config(project)
        process = subprocess.Popen([str(project / "Game.exe"), "-3dfx", "-direct", "-txt"],
                                   cwd=project, close_fds=True)
        return process.pid
