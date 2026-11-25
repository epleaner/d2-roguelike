"""Reconstruct one published, pinned Store revision from player-owned authenticated MPQs.

The public plan contains Store operations and hashes, never source tables or assets.
The returned directory is staged data, not a game installation.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import mod_archive
import mod_data
import mod_strings
from standalone import ds1_reconstruct, map_recipes

PINNED_SOURCE = {"path": r"C:\D2Lab\working\pd2-s14-patch2\game\ProjectD2\pd2data.mpq",
                 "sha256": "7e41718ef7b737464d13ee82f4e7093c63bc3797eed6f76953e1c7b406e08d09",
                 "bytes": 2221781}
PATCHSTRING_SHA256 = "7e7b1d09456bc94e16a9bf92838fe57d18ce2373003b4730e2360e6a5da4cb0d"
CLASSIC_SHA256 = "e70adad3aa23f1f72ecd53f01a2cfd9ae1aa7ac043549cb4c881c2d30a47a1af"
TOWER_SOURCE_SHA256 = "432c7d801466208657b90d75e27c2f10cbc3a79fb10790c193ba9452e4fe28a6"
TOWER_OUTPUT_SHA256 = "b729b7f793ef816c9cd86d36ceeb805c17a1e738fd9d7314b5c92a78817f1de1"
ARENA_PATH = "data/global/tiles/Act1/Outdoors/RogueTower13.ds1"
TOWER_MEMBER = r"data\global\tiles\Act1\Outdoors\Tower2.ds1"
_ARENA_RECORD = {"path": ARENA_PATH, "bytes": 1383, "sha256": TOWER_OUTPUT_SHA256}
_MAP_RECIPES = map_recipes.DATA["recipes"]
_SEALED_TOWN_MAPS = frozenset(
    f"data/global/tiles/Act1/Town/Town{direction}1.ds1" for direction in "NESW"
)
_MAP_RECORDS = [_ARENA_RECORD, *(recipe["output"] for recipe in _MAP_RECIPES.values())]
_MAP_RECORDS.sort(key=lambda record: record["path"])
_ARCHIVE_BYTES = {"d2data.mpq": 269191615, "d2exp.mpq": 250156780,
                  "pd2maps.mpq": 5240233}
_PATCH_ARCHIVES = {2108703: "b976e7847dda696f54198a9477819f96579c4894f54381c72afb4937e919e0a9",
                   3915305: "81470b762c497a8fcaf9a09990c0e869a4175395ef1fae13bd85cfec7b688ddc"}
_REVISION_FIELDS = frozenset({"schema_version", "revision", "baseline_id", "files", "changes",
                              "localization_source_id", "recipes", "unique_items", "skills",
                              "monsters", "visual_assets", "row_templates"})


def _file_record(record, path_check):
    if (not isinstance(record, dict) or set(record) != {"path", "sha256", "bytes"} or
            not isinstance(record["path"], str)):
        raise ValueError("Invalid plan file record")
    path_check(record["path"])
    if (not isinstance(record["sha256"], str) or not mod_data._HASH.fullmatch(record["sha256"]) or
            type(record["bytes"]) is not int or record["bytes"] <= 0):
        raise ValueError("Invalid plan file identity")


def _inventory(files, path_check):
    if not isinstance(files, list) or not files:
        raise ValueError("Invalid plan inventory")
    for record in files:
        _file_record(record, path_check)
    if files != sorted(files, key=lambda item: item["path"]):
        raise ValueError("Invalid plan inventory")
    if len({record["path"].casefold() for record in files}) != len(files):
        raise ValueError("Duplicate or case-colliding output path")



def _localization_path(path):
    if path != mod_data._PATCHSTRING:
        raise ValueError("Invalid localization path")



def _validated_baseline(plan):
    if (not isinstance(plan, dict) or
            set(plan) != {"schema_version", "baseline", "revision", "classic_sha256", "sha256"} or
            type(plan["schema_version"]) is not int or plan["schema_version"] != 1):
        raise ValueError("Unsupported data plan schema")
    if plan["sha256"] != mod_data._digest({key: value for key, value in plan.items() if key != "sha256"}):
        raise ValueError("Plan digest mismatch")
    baseline, revision = plan["baseline"], plan["revision"]
    if not isinstance(baseline, dict) or not isinstance(revision, dict):
        raise ValueError("Invalid plan metadata")
    fields = {"schema_version", "baseline_id", "source_archives", "files"}
    if set(baseline) not in (fields, fields | {"localization_source"}):
        raise ValueError("Invalid baseline manifest")
    if (type(baseline["schema_version"]) is not int or baseline["schema_version"] != 1 or
            baseline["source_archives"] != [PINNED_SOURCE] or plan["classic_sha256"] != CLASSIC_SHA256):
        raise ValueError("Plan is not pinned to S14 patch #2 and classic LoD")
    _inventory(baseline["files"], mod_data._table)
    mod_data._identifier(baseline["baseline_id"], "base_")
    identity = {key: baseline[key] for key in ("schema_version", "source_archives", "files")}
    if baseline["baseline_id"] != "base_" + mod_data._digest(identity):
        raise ValueError("Baseline identity mismatch")
    source = baseline.get("localization_source")
    if source is not None:
        if (not isinstance(source, dict) or set(source) != {"id", "path", "member", "source_archive", "sha256", "bytes"} or
                source["path"] != mod_data._PATCHSTRING or source["member"] != mod_data._PATCHSTRING_MEMBER or
                source["source_archive"] != PINNED_SOURCE or source["sha256"] != PATCHSTRING_SHA256):
            raise ValueError("Invalid localization source")
        _file_record({key: source[key] for key in ("path", "sha256", "bytes")}, _localization_path)
        if source["id"] != mod_data._localization_source_identity(baseline["baseline_id"], source):
            raise ValueError("Localization identity mismatch")
    return baseline, revision


def _validate_revision(baseline, revision):
    if (not {"schema_version", "revision", "baseline_id", "files", "changes"} <= set(revision) or
            not set(revision) <= _REVISION_FIELDS or type(revision["schema_version"]) is not int or
            revision["schema_version"] != 1 or revision["baseline_id"] != baseline["baseline_id"]):
        raise ValueError("Invalid revision metadata")
    mod_data._identifier(revision["revision"], "mod_")
    assets = revision.get("visual_assets", [])
    if revision.get("monsters") or assets not in ([], [_ARENA_RECORD], _MAP_RECORDS):
        raise ValueError("Plan includes non-derivable game assets")
    if "monsters" in revision or any(field in revision and not revision[field] for field in
                                      ("recipes", "unique_items", "skills", "visual_assets", "row_templates")):
        raise ValueError("Invalid revision operations")
    mod_data.Store._validate_changes(revision["changes"], baseline)
    mod_data._validated_row_templates(revision.get("row_templates", []), baseline)
    localization_id = mod_data.Store._revision_localization_id(revision, baseline,
        revision.get("unique_items", []), revision.get("skills", []), [], revision.get("row_templates", []))
    if revision["revision"] != "mod_" + mod_data._digest(mod_data.Store._revision_identity(revision, localization_id)):
        raise ValueError("Revision identity mismatch")
    asset_records = {record["path"]: record for record in assets}
    _inventory(revision["files"], lambda path: None if path == mod_data._PATCHSTRING and localization_id else
               (None if path in asset_records else mod_data._table(path)))
    expected = [record["path"] for record in baseline["files"]]
    if localization_id:
        expected.append(mod_data._PATCHSTRING)
    expected.extend(asset_records)
    if [record["path"] for record in revision["files"]] != sorted(expected):
        raise ValueError("Revision inventory mismatch")


def _validated(plan):
    baseline, revision = _validated_baseline(plan)
    _validate_revision(baseline, revision)
    return baseline, revision


def export_plan(store_root: Path, revision: str) -> dict:
    """Export only an existing immutable Store revision, never a workspace head."""
    store = mod_data.Store(store_root)
    baseline = store.inspect()
    published = store.inspect(revision=revision)
    metadata = {key: value for key, value in published.items() if key not in ("name", "source_archives")}
    plan = {"schema_version": 1, "baseline": baseline, "revision": metadata,
            "classic_sha256": CLASSIC_SHA256}
    plan["sha256"] = mod_data._digest(plan)
    _validated(plan)
    return plan


class _AuthenticatedRender(mod_data.Store):
    """Supply source bytes to the existing Store renderer without creating a Store."""
    def __init__(self, tables, localization, assets, staging_root):
        # Store builds asset paths before calling our in-memory override.
        super().__init__(staging_root)
        self.tables, self.localization, self.assets = tables, localization, assets

    def _baseline_bytes(self, baseline):
        return dict(self.tables)

    def _source_localization_bytes(self, baseline):
        if self.localization is None:
            raise ValueError("Authenticated English localization is missing")
        return self.localization

    def _checked_file(self, path, record):
        expected = _ARENA_RECORD if record["path"] == ARENA_PATH else _MAP_RECIPES[record["path"]]["output"]
        if record != expected or record["path"] not in self.assets:
            raise ValueError("Unexpected game asset")
        return self.assets[record["path"]]


def _arena_from_tower(original):
    if (len(original) != 1383 or mod_data._hash(original) != TOWER_SOURCE_SHA256 or
            original[439] != 10 or original[475] != 10):
        raise ValueError("Authenticated classic Tower2 identity differs")
    arena = bytearray(original)
    arena[439] = arena[475] = 12
    if mod_data._hash(arena) != TOWER_OUTPUT_SHA256:
        raise ValueError("Derived arena identity differs")
    return bytes(arena)


def _arena_from_classic(path):
    with mod_archive._authenticated_archive(path, CLASSIC_SHA256) as read:
        return _arena_from_tower(read(TOWER_MEMBER))


def _map_assets(classic_archive, expansion_archive, patch_archive, maps_archive):
    paths = {"d2data.mpq": classic_archive, "d2exp.mpq": expansion_archive,
             "patch_d2.mpq": patch_archive, "pd2maps.mpq": maps_archive}
    if os.name != "nt":
        raise ValueError("Authenticated S14 map archive extraction requires the Windows StormLib backend")
    assets = {}
    for archive, path in paths.items():
        if path is None:
            raise ValueError("Required authenticated map archive is missing: " + archive)
        size = Path(path).stat().st_size
        digest = (_PATCH_ARCHIVES.get(size) if archive == "patch_d2.mpq" else
                  map_recipes.DATA["archives"][archive]
                  if size == _ARCHIVE_BYTES[archive] else None)
        if digest is None:
            raise ValueError("Required authenticated map archive differs: " + archive)
        with mod_archive._authenticated_archive(path, digest) as read:
            if archive == "d2data.mpq":
                assets[ARENA_PATH] = _arena_from_tower(read(TOWER_MEMBER))
            for output, recipe in _MAP_RECIPES.items():
                if recipe["archive"] != archive:
                    continue
                source = read(recipe["member"].replace("/", "\\"))
                if (len(source), mod_data._hash(source)) != (recipe["sourceBytes"], recipe["sourceSha"]):
                    raise ValueError("Authenticated map member differs: " + output)
                derived = ds1_reconstruct.reconstruct(source, recipe["operation"], recipe["walls"])
                if output in _SEALED_TOWN_MAPS:
                    derived = ds1_reconstruct.seal_town(derived)
                record = recipe["output"]
                if (len(derived), mod_data._hash(derived)) != (record["bytes"], record["sha256"]):
                    raise ValueError("Derived map differs from pinned revision: " + output)
                assets[output] = derived
    return assets


def render_plan(plan: dict, pd2_archive: Path, classic_archive: Path, output_dir: Path, *,
                expansion_archive: Path | None = None, patch_archive: Path | None = None,
                maps_archive: Path | None = None) -> dict:
    """Preflight every source and output byte before writing a fresh staging directory."""
    baseline, revision = _validated(plan)
    output_dir = Path(output_dir).absolute()
    if output_dir.exists() or output_dir.is_symlink():
        raise ValueError("Output directory must be fresh")
    for parent in output_dir.parents:
        if parent.is_symlink():
            raise ValueError("Output directory cannot have symlink ancestors")
    if mod_archive.sha256(pd2_archive) != PINNED_SOURCE["sha256"] or Path(pd2_archive).stat().st_size != PINNED_SOURCE["bytes"]:
        raise ValueError("PD2 archive is not the pinned S14 patch #2 source")
    tables = mod_archive.read_tables(pd2_archive, PINNED_SOURCE["sha256"])
    inventory = {record["path"]: record for record in baseline["files"]}
    if set(tables) != set(inventory):
        raise ValueError("Authenticated PD2 table inventory differs")
    for path, data in tables.items():
        if (len(data), mod_data._hash(data)) != (inventory[path]["bytes"], inventory[path]["sha256"]):
            raise ValueError("Authenticated PD2 source table differs: " + path)
    if not revision.get("visual_assets") and mod_archive.sha256(classic_archive) != CLASSIC_SHA256:
        raise ValueError("Classic archive is not the authenticated LoD source")
    localization = None
    if "localization_source" in baseline:
        localization = mod_archive.read_english_patchstring(pd2_archive, PINNED_SOURCE["sha256"])
        source = baseline["localization_source"]
        if (len(localization), mod_data._hash(localization)) != (source["bytes"], source["sha256"]):
            raise ValueError("Authenticated PD2 localization differs")
        mod_strings._parse(localization)
    assets = (_map_assets(classic_archive, expansion_archive, patch_archive, maps_archive)
              if len(revision.get("visual_assets", [])) == len(_MAP_RECORDS)
              else {ARENA_PATH: _arena_from_classic(classic_archive)}
              if revision.get("visual_assets") else {})
    renderer = _AuthenticatedRender(tables, localization, assets, output_dir)
    rendered = renderer._render(baseline, revision["changes"], revision.get("recipes", []),
        revision.get("unique_items", []), revision.get("skills", []), [],
        legacy="localization_source_id" not in revision,
        visual_assets=revision.get("visual_assets", []), row_templates=revision.get("row_templates", []))
    files = revision["files"]
    if set(rendered) != {record["path"] for record in files}:
        raise ValueError("Rendered inventory differs from published revision")
    for record in files:
        data = rendered[record["path"]]
        if (len(data), mod_data._hash(data)) != (record["bytes"], record["sha256"]):
            raise ValueError("Rendered output differs from published revision: " + record["path"])
    output_dir.mkdir(parents=True, exist_ok=False)
    for record in files:
        target = output_dir / record["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(rendered[record["path"]])
    return {"revision": revision["revision"], "files": files}
