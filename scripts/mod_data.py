"""Private, lossless PD2 table workspaces. No archive extraction or game deployment.

All paths below ``root`` are owned by this store. Mutations use an interprocess
lock and workspace CAS. Source
archives are provenance supplied by the authenticated extractor; this module
checks their metadata shape, not the archive contents. Numeric checks only cover
lexical numeric cells; engine formulas and table-specific schemas are not validated.
"""
from __future__ import annotations

from contextlib import contextmanager
from functools import wraps
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

import mod_strings

if os.name == "nt":
    import msvcrt
else:
    import fcntl


_TABLE = re.compile(r"data/global/excel/[A-Za-z0-9_-]+\.txt\Z")
_ROW_TEMPLATE_KEYS = {
    "data/global/excel/levels.txt": "Id",
    "data/global/excel/treasureclassex.txt": "Treasure Class",
    "data/global/excel/lvlprest.txt": "Def",
    "data/global/excel/misc.txt": "code",
}
_PATCHSTRING = "data/local/LNG/ENG/patchstring.tbl"
_LEGACY_TREASURECLASS = "data/global/excel/treasureclass.txt"
_LEGACY_TREASURE_KEY = "LegacyTreasureClass"
_PATCHSTRING_MEMBER = r"data\local\LNG\ENG\patchstring.tbl"
_NAME = re.compile(r"[a-z][a-z0-9-]{1,31}\Z")
_HASH = re.compile(r"[0-9a-f]{64}\Z")
_NUMBER = re.compile(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\Z")
_MONSTER_ASSET_MODES = ("NU", "WL", "GH", "A1", "A2", "DT", "DD")
_MONSTER_ASSET_ROOT = "data/global/monsters/{token}"
_MONSTER_ANIMDATA = "data/global/AnimData.d2"
_VISUAL_ASSET_PATHS = frozenset({
    "data/global/items/invhaxhf.dc6",
    "data/global/items/invxtphf.dc6",
    "data/global/items/invjewel.dc6",
    "data/global/ui/spells/BaSkillicon.DC6",
    "data/global/tiles/Act1/Outdoors/RogueTower13.ds1",
    "data/global/tiles/Act1/Outdoors/RogueTower14.ds1",
    "data/global/tiles/Act1/Outdoors/RogueTower15.ds1",
    "data/global/tiles/Act1/Outdoors/RogueTower16.ds1",
})


_IDENTITY = {"id", "hcidx", "code"}
_HYDRA_PALETTE_TREE_SLOTS = (
    (2, 2, 1), (2, 2, 2), (2, 2, 3), (2, 3, 3),
    (2, 4, 1), (2, 4, 2), (2, 5, 3), (2, 6, 1),
)


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii")


def _digest(value):
    return hashlib.sha256(_canonical(value)).hexdigest()


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def _table(path):
    if not isinstance(path, str) or not _TABLE.fullmatch(path):
        raise ValueError("Unsafe table path; expected data/global/excel/<name>.txt")
    return path


def _name(name):
    if not isinstance(name, str) or not _NAME.fullmatch(name):
        raise ValueError("Workspace name must be 2–32 lowercase safe characters")
    return name


def _identifier(value, prefix):
    if not isinstance(value, str) or not value.startswith(prefix) or not _HASH.fullmatch(value[len(prefix):]):
        raise ValueError("Invalid content identifier")
    return value


def _cell(value):
    if not isinstance(value, str) or any(c in value for c in "\t\r\n\0"):
        raise ValueError("Cell must be text without tab, newline, or NUL")
    return value.encode("cp1252")


def _atomic_json(path, value):
    data = _canonical(value)
    fd, temporary = tempfile.mkstemp(prefix=".update-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _atomic_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".asset-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _expected_monster_asset_paths(token):
    root = _MONSTER_ASSET_ROOT.format(token=token)
    return ({f"{root}/TR/{token}TRLIT{mode}HTH.dcc" for mode in _MONSTER_ASSET_MODES} |
            {f"{root}/COF/{token.lower()}{mode.lower()}hth.cof" for mode in _MONSTER_ASSET_MODES} |
            {_MONSTER_ANIMDATA})


def _validated_monster_asset_records(records, token):
    expected = _expected_monster_asset_paths(token)
    if not isinstance(records, list) or len(records) != len(expected):
        raise ValueError("Monster asset manifest must contain exactly 15 files")
    checked = []
    for record in records:
        if (not isinstance(record, dict) or set(record) != {"path", "sha256", "bytes"} or
                not isinstance(record["path"], str) or record["path"] not in expected or
                not isinstance(record["sha256"], str) or not _HASH.fullmatch(record["sha256"]) or
                type(record["bytes"]) is not int or record["bytes"] < 0):
            raise ValueError("Invalid monster asset file record")
        checked.append(dict(record))
    if {record["path"] for record in checked} != expected:
        raise ValueError("Monster asset manifest has an incomplete asset set")
    return sorted(checked, key=lambda record: record["path"])


def _validated_visual_asset_record(record):
    if (not isinstance(record, dict) or set(record) != {"path", "sha256", "bytes"} or
            not isinstance(record["path"], str) or
            record["path"] not in _VISUAL_ASSET_PATHS or
            not isinstance(record["sha256"], str) or not _HASH.fullmatch(record["sha256"]) or
            type(record["bytes"]) is not int or record["bytes"] <= 0):
        raise ValueError("Invalid visual asset file record")
    return dict(record)


def _validated_visual_asset_records(records):
    if not isinstance(records, list) or not 1 <= len(records) <= len(_VISUAL_ASSET_PATHS):
        raise ValueError("Visual asset manifest must contain one or more allowlisted files")
    checked = [_validated_visual_asset_record(record) for record in records]
    if len({record["path"] for record in checked}) != len(checked):
        raise ValueError("Duplicate visual asset path")
    return sorted(checked, key=lambda record: record["path"])


def _read_asset_payloads(package_dir, records, asset_kind):
    payloads = {}
    for record in records:
        path = package_dir / record["path"]
        current = package_dir
        for part in Path(record["path"]).parts:
            current = current / part
            if current.is_symlink():
                raise ValueError(f"Symlink in {asset_kind} asset package")
        if not path.is_file():
            raise ValueError(f"{asset_kind.capitalize()} asset package file is missing")
        data = path.read_bytes()
        if len(data) != record["bytes"] or _hash(data) != record["sha256"]:
            raise ValueError(f"{asset_kind.capitalize()} asset package hash mismatch")
        payloads[record["path"]] = data
    return payloads


def _read_visual_asset_package(package_dir):
    package_dir = Path(package_dir)
    manifest_path = package_dir / "asset-manifest.json"
    if package_dir.is_symlink() or not package_dir.is_dir() or manifest_path.is_symlink():
        raise ValueError("Invalid or symlinked visual asset package")
    manifest = _load_json(manifest_path)
    if (not isinstance(manifest, dict) or
            manifest.get("target") != "Project Diablo II S13 patch #2, LoD engine; not D2R"):
        raise ValueError("Visual asset package target mismatch")
    records = _validated_visual_asset_records(manifest.get("files"))
    return records, _read_asset_payloads(package_dir, records, "visual")


def _read_monster_asset_payloads(package_dir, records):
    return _read_asset_payloads(package_dir, records, "monster")

def _read_monster_asset_package(package_dir, token):
    package_dir = Path(package_dir)
    manifest_path = package_dir / "asset-manifest.json"
    if package_dir.is_symlink() or not package_dir.is_dir() or manifest_path.is_symlink():
        raise ValueError("Invalid or symlinked monster asset package")
    manifest = _load_json(manifest_path)
    if (not isinstance(manifest, dict) or
            manifest.get("target") != "Project Diablo II S13 patch #2, LoD engine; not D2R" or
            manifest.get("token") != token or manifest.get("component") != "TR" or
            manifest.get("modes") != list(_MONSTER_ASSET_MODES) or
            manifest.get("directions") != 8):
        raise ValueError("Monster asset package target or animation layout mismatch")
    records = _validated_monster_asset_records(manifest.get("files"), token)
    return records, _read_monster_asset_payloads(package_dir, records)


def _load_json(path):
    return json.loads(path.read_bytes())


def _safe_store_path(self, path):
    path = Path(path)
    try:
        relative = path.relative_to(self.root)
    except ValueError as error:
        raise ValueError("Path escapes store") from error
    current = self.root
    if current.is_symlink():
        raise ValueError("Symlink in store")
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError("Symlink in store")
    return path


def _records(data):
    """Split only on CR/LF record endings; other CP1252 control bytes are cell data."""
    records = []
    start = index = 0
    while index < len(data):
        byte = data[index]
        if byte not in (10, 13):
            index += 1
            continue
        end = index + 1
        if byte == 13 and end < len(data) and data[end] == 10:
            end += 1
        records.append(data[start:end])
        start = index = end
    if start < len(data):
        records.append(data[start:])
    return records


def _legacy_treasure_row(lines, key, column):
    """Address a bounded cell in the headerless classic treasure table."""
    if column == "NoDrop":
        cell_index = 6
    else:
        match = re.fullmatch(r"(Item|Prob)(10|[1-9])", column)
        if not match:
            raise ValueError("Unexpected legacy treasure class column")
        slot = int(match[2])
        cell_index = 7 + 2 * (slot - 1) + int(match[1] == "Prob")
    if (len(lines) != 707 or
            lines[0].split(b"\t", 1)[0] != b"Gold"):
        raise ValueError("Unexpected legacy treasure class layout")
    wanted = _cell(key)
    found = []
    for index, line in enumerate(lines):
        cells = line.rstrip(b"\r\n").split(b"\t")
        if len(cells) != 28:
            raise ValueError("Unexpected legacy treasure class row width")
        if cells[0] == wanted:
            found.append((index, cells))
    if len(found) != 1:
        raise ValueError("Missing or duplicate legacy treasure class")
    index, cells = found[0]
    return lines, index, cell_index, cells[cell_index]


def _row(data, key_column, key, column):
    """Find one row; return its byte lines, index, column indices and current cell."""
    lines = _records(data)
    if not lines:
        raise ValueError("Empty table")
    if key_column == _LEGACY_TREASURE_KEY:
        return _legacy_treasure_row(lines, key, column)
    header = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
    if header.count(key_column) != 1 or header.count(column) != 1:
        raise ValueError("Missing or duplicate key/target column")
    ki, ci = header.index(key_column), header.index(column)
    matches = []
    wanted = _cell(key)
    for index, line in enumerate(lines[1:], 1):
        cells = line.rstrip(b"\r\n").split(b"\t")
        if len(cells) <= ki:
            continue
        if cells[ki] == wanted:
            matches.append((index, cells))
    if len(matches) != 1:
        raise ValueError("Missing or duplicate row key")
    index, cells = matches[0]
    if len(cells) != len(header):
        raise ValueError("Target row does not match header layout")
    return lines, index, ci, cells[ci]


def _replace(data, change, expected):
    lines, index, ci, current = _row(data, change["key_column"], change["key"], change["column"])
    if current != _cell(expected):
        raise ValueError("Expected cell differs from current value")
    if (_NUMBER.fullmatch(current.decode("cp1252")) and
            not _NUMBER.fullmatch(change["value"]) and
            not (change["table"] == "data/global/excel/charstats.txt" and
                 change["column"] == "item6" and current == b"0" and
                 change["value"] == "rid")):
        raise ValueError("Numeric cell requires numeric replacement")
    line = lines[index]
    ending = b"\r\n" if line.endswith(b"\r\n") else b"\n" if line.endswith(b"\n") else b"\r" if line.endswith(b"\r") else b""
    cells = line[:-len(ending)].split(b"\t") if ending else line.split(b"\t")
    cells[ci] = _cell(change["value"])
    lines[index] = b"\t".join(cells) + ending
    return b"".join(lines)

def _template_rows(data, operation):
    """Copy one authenticated source row into bounded, distinctly keyed rows."""
    lines = _records(data)
    header = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
    key_column = operation["key_column"]
    if len(header) != len(set(header)) or key_column not in header:
        raise ValueError("Missing or ambiguous row-template key column")
    indexes = {column: index for index, column in enumerate(header)}
    key_index = indexes[key_column]
    rows = [line.rstrip(b"\r\n").decode("cp1252").split("\t") for line in lines[1:]]
    if any(len(row) != len(header) for row in rows):
        raise ValueError("Malformed template table row")
    keys = [row[key_index] for row in rows]
    if len(keys) != len(set(keys)):
        raise ValueError("Template table keys must be unique")
    template = [row for row in rows if row[key_index] == operation["template_key"]]
    if len(template) != 1:
        raise ValueError("Template source row missing")
    positions = {key: index for index, key in enumerate(keys)}
    additions = []
    seen_targets = set()
    for mode in ("updates", "additions"):
        for entry in operation[mode]:
            key = entry["key"]
            if key in seen_targets:
                raise ValueError("Duplicate template target key")
            seen_targets.add(key)
            exists = key in positions
            if (mode == "updates") != exists or key == operation["template_key"]:
                raise ValueError("Template target existence mismatch")
            cells = entry["cells"]
            if not isinstance(cells, dict) or not cells or key_column in cells or (
                    set(cells) - indexes.keys()):
                raise ValueError("Invalid template overrides")
            for column, value in cells.items():
                _cell(column)
                _cell(value)
            values = template[0].copy()
            values[key_index] = key
            for column, value in cells.items():
                values[indexes[column]] = value
            encoded = b"\t".join(_cell(value) for value in values)
            if exists:
                index = positions[key]
                ending = b"\r\n" if lines[index + 1].endswith(b"\r\n") else b"\n"
                lines[index + 1] = encoded + ending
            else:
                additions.append(encoded)
    if additions:
        ending = b"\r\n" if lines[0].endswith(b"\r\n") else b"\n"
        if rows[-1][key_index] == "" and "Name" in indexes and (
                rows[-1][indexes["Name"]] == "Expansion"):
            lines[-1:-1] = [row + ending for row in additions]
        else:
            if not lines[-1].endswith((b"\r", b"\n")):
                lines.append(ending)
            lines.extend(row + ending for row in additions)
    return b"".join(lines)


def _validated_row_templates(operations, baseline):
    if not isinstance(operations, list) or len(operations) > 4:
        raise ValueError("At most four row-template operations allowed")
    inventory = {file["path"] for file in baseline["files"]}
    paths = []
    for operation in operations:
        if not isinstance(operation, dict) or set(operation) != {
                "table", "key_column", "template_key", "updates", "additions"}:
            raise ValueError("Invalid row-template operation")
        path = _table(operation["table"])
        if path not in inventory or operation["key_column"] != _ROW_TEMPLATE_KEYS.get(path):
            raise ValueError("Only keyed Levels, LvlPrest, TreasureClassEx, and Misc source rows can be cloned")
        paths.append(path)
        for field in ("key_column", "template_key"):
            _cell(operation[field])
            if not operation[field]:
                raise ValueError("Empty template selector")
        for mode in ("updates", "additions"):
            entries = operation[mode]
            if not isinstance(entries, list) or len(entries) > 128:
                raise ValueError("Row-template batch exceeds 128 rows")
            for entry in entries:
                allowed = {"key", "cells"}
                if (mode == "additions" and
                        path == "data/global/excel/misc.txt"):
                    allowed.add("localizations")
                if (not isinstance(entry, dict) or
                        not {"key", "cells"}.issubset(entry) or
                        set(entry) - allowed):
                    raise ValueError("Invalid template target")
                _cell(entry["key"])
                if not entry["key"]:
                    raise ValueError("Empty template target key")
        if not operation["updates"] and not operation["additions"]:
            raise ValueError("Empty row-template operation")
        if path == "data/global/excel/misc.txt":
            for entry in operation["additions"]:
                cells = entry["cells"]
                key = entry["key"]
                if (not _ITEM_CODE.fullmatch(key) or
                        cells.get("namestr") != f"d2lab_misc_{key}_name" or
                        not cells.get("*name") or cells.get("name") != cells["*name"]):
                    raise ValueError("Misc additions require exact keyed English localization")
                _localized_text(cells["*name"], "misc item name", 63)
                localizations = entry.get("localizations")
                if localizations is not None:
                    description_key = f"d2lab_misc_{key}_desc"
                    if (not isinstance(localizations, list) or
                            len(localizations) != 1 or
                            not isinstance(localizations[0], dict) or
                            set(localizations[0]) != {"key", "value"} or
                            localizations[0]["key"] != description_key or
                            cells.get("spelldesc") != "1" or
                            cells.get("spelldescstr") != description_key):
                        raise ValueError(
                            "Misc helper copy requires exact spell-description localization")
                    helper_copy = localizations[0]["value"]
                    multiline = (
                        isinstance(helper_copy, str) and "\n" in helper_copy)
                    _localized_text(helper_copy, "misc item helper copy", 256,
                                    multiline=multiline)
            if operation["updates"]:
                raise ValueError("Misc templates cannot replace stock items")
    if paths != sorted(set(paths)):
        raise ValueError("Duplicate or unsorted row-template tables")
    return operations

_CUBE = "data/global/excel/cubemain.txt"
_BASE_ITEM_TABLES = ("misc.txt", "armor.txt", "weapons.txt")
_ITEM_CODE = re.compile(r"[a-z0-9]{3,4}\Z")
_RECIPE_DESCRIPTION = re.compile(r"[A-Za-z0-9][A-Za-z0-9 _-]{1,63}\Z")
_UNIQUE_ITEMS = "data/global/excel/uniqueitems.txt"
_PROPERTIES = "data/global/excel/properties.txt"
_ITEMSTATCOST = "data/global/excel/itemstatcost.txt"
_UNIQUE_STATS = {"str": "strength", "dex": "dexterity", "vit": "vitality", "enr": "energy"}
_SKILLS = "data/global/excel/skills.txt"
_SKILLDESC = "data/global/excel/skilldesc.txt"
_MISSILES = "data/global/excel/missiles.txt"
_MONSTATS = "data/global/excel/monstats.txt"
_MONSTATS2 = "data/global/excel/monstats2.txt"
_MONSTER_TOKEN = re.compile(r"[A-Z]{2}\Z")

_COLD_HYDRA_TRANSLEVEL = "0"
_SKILL_NAME = re.compile(r"[A-Za-z][A-Za-z0-9 '()-]{1,63}\Z")

_LEGACY_UNIQUE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9 '()-]{1,63}\Z")


def _recipe_spec(recipe):
    if not isinstance(recipe, dict) or set(recipe) != {"description", "inputs", "output"}:
        raise ValueError("Recipe requires description, inputs, and output")
    description, inputs, output = recipe["description"], recipe["inputs"], recipe["output"]
    if not isinstance(description, str) or not _RECIPE_DESCRIPTION.fullmatch(description):
        raise ValueError("Recipe description must be 2–64 ASCII letters, digits, spaces, underscores, or hyphens")
    _cell(description)
    if not isinstance(inputs, list) or not 1 <= len(inputs) <= 7:
        raise ValueError("Recipe requires 1–7 exact-code inputs")
    normalized, codes = [], set()
    for item in inputs:
        if not isinstance(item, dict) or set(item) != {"code", "quantity"}:
            raise ValueError("Recipe inputs require only code and quantity")
        code, quantity = item["code"], item["quantity"]
        if not isinstance(code, str) or not _ITEM_CODE.fullmatch(code):
            raise ValueError("Recipe item code must be a lowercase 3–4 character base code")
        if type(quantity) is not int or not 1 <= quantity <= 255:
            raise ValueError("Recipe input quantity must be an integer from 1 to 255")
        if code in codes:
            raise ValueError("Recipe input codes must be unique; use quantity for repeats")
        codes.add(code)
        normalized.append({"code": code, "quantity": quantity})
    if not isinstance(output, str) or not _ITEM_CODE.fullmatch(output):
        raise ValueError("Recipe output must be a lowercase 3–4 character base code")
    return {"description": description, "inputs": sorted(normalized, key=lambda item: item["code"]),
            "output": output}


def _recipe_identity(recipe):
    return tuple((item["code"], item["quantity"]) for item in recipe["inputs"])


def _unique_item_spec(item):
    required = {"name", "base_code", "level", "required_level", "stat", "value"}
    if not isinstance(item, dict) or set(item) != required:
        raise ValueError("Unique item requires name, base_code, level, required_level, stat, and value")
    name, code = item["name"], item["base_code"]
    level, required_level, stat, value = (
        item["level"], item["required_level"], item["stat"], item["value"])
    if not isinstance(name, str):
        raise ValueError("Unique item name must be text")
    name = _localized_text(name, "Unique item name", 64)
    if not isinstance(code, str) or not _ITEM_CODE.fullmatch(code):
        raise ValueError("Unique item base_code must be a lowercase 3–4 character item code")
    if type(level) is not int or not 1 <= level <= 99:
        raise ValueError("Unique item level must be an integer from 1 to 99")
    if type(required_level) is not int or not 0 <= required_level <= level:
        raise ValueError("Unique item required_level must be an integer from 0 through level")
    if not isinstance(stat, str) or stat not in _UNIQUE_STATS:
        raise ValueError("Unique item stat must be one of str, dex, vit, or enr")
    if type(value) is not int or not 1 <= value <= 100:
        raise ValueError("Unique item stat value must be an integer from 1 to 100")
    return {"name": name, "base_code": code, "level": level,
            "required_level": required_level, "stat": stat, "value": value}

def _monster_spec(monster):
    if not isinstance(monster, dict) or set(monster) not in ({"name", "token"},
                                                              {"name", "token", "asset_files"}):
        raise ValueError("Monster requires name and token")
    name = _localized_text(monster["name"], "Monster name", 64)
    token = monster["token"]
    if not isinstance(token, str) or not _MONSTER_TOKEN.fullmatch(token):
        raise ValueError("Monster token must be two uppercase ASCII letters")
    normalized = {"name": name, "token": token}
    if "asset_files" in monster:
        normalized["asset_files"] = _validated_monster_asset_records(monster["asset_files"], token)
    return normalized


def _legacy_unique_item_spec(item):
    required = {"name", "base_code", "level", "required_level", "stat", "value"}
    if not isinstance(item, dict) or set(item) != required:
        raise ValueError("Unique item requires name, base_code, level, required_level, stat, and value")
    name = item["name"]
    if not isinstance(name, str) or not _LEGACY_UNIQUE_NAME.fullmatch(name):
        raise ValueError(
            "Unique item name must be 2–64 ASCII letters, digits, spaces, apostrophes, parentheses, or hyphens")
    _cell(name)
    return _unique_item_spec(item)


def _localized_text(value, field, maximum, multiline=False):
    has_invalid_control = (
        any(ord(char) < 0x20 and not (multiline and char == "\n")
            for char in value) if isinstance(value, str) else True)
    if (not isinstance(value, str) or not value.strip() or
            not 2 <= len(value) <= maximum or has_invalid_control or
            (multiline and ("\n" not in value or value.count("\n") != 1 or
                            any(not line.strip() for line in value.split("\n"))))):
        raise ValueError(f"{field} must be 2–{maximum} printable characters")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as error:
        raise ValueError(f"{field} is not valid Unicode text") from error
    if len(encoded) > 1024:
        raise ValueError(f"{field} exceeds the supported encoded length")
    return value


def _skill_spec(skill):
    required = {"name", "short_description", "description"}
    if (not isinstance(skill, dict) or not required.issubset(skill) or
            set(skill) - required - {"template", "monster_trans_level"}):
        raise ValueError("New skill requires name, short_description, and description, plus an optional supported template setting")
    template = skill.get("template", "fire_bolt")
    if template not in ("fire_bolt", "cold_hydra"):
        raise ValueError("Skill template must be fire_bolt or cold_hydra")
    name = skill["name"]
    if not isinstance(name, str) or not _SKILL_NAME.fullmatch(name):
        raise ValueError("New skill name must be 2–64 ASCII letters, digits, spaces, apostrophes, parentheses, or hyphens")
    _cell(name)
    normalized = {"name": name,
                  "short_description": _localized_text(skill["short_description"], "Short description", 128),
                  "description": _localized_text(skill["description"], "Description", 256)}
    if "template" in skill:
        normalized["template"] = template
    if "monster_trans_level" in skill:
        if template != "cold_hydra":
            raise ValueError("monster_trans_level is supported only by the cold_hydra template")
        trans_level = skill["monster_trans_level"]
        if type(trans_level) is not int or not 0 <= trans_level <= 7:
            raise ValueError("Cold Hydra monster_trans_level must be an integer from 0 to 7")
        normalized["monster_trans_level"] = trans_level
    return normalized

def _legacy_skill_spec(skill):
    if not isinstance(skill, dict) or set(skill) != {"name"}:
        raise ValueError("New skill definition requires only name")
    name = skill["name"]
    if not isinstance(name, str) or not _SKILL_NAME.fullmatch(name):
        raise ValueError(
            "New skill name must be 2–64 ASCII letters, digits, spaces, apostrophes, parentheses, or hyphens")
    _cell(name)
    return {"name": name}

def _unique_string_key(index):
    return f"d2lab_unique_{index}_name"


def _skill_string_keys(skill_id):
    prefix = f"d2lab_skill_{skill_id}_"
    return {field: prefix + field for field in ("name", "short", "long", "alt")}


def _localization_source_identity(baseline_id, source):
    fields = {key: source[key] for key in ("path", "member", "source_archive", "sha256", "bytes")}
    return "locsrc_" + _digest({"baseline_id": baseline_id, **fields})

def _tabular_rows(data, label, unique_columns=()):
    lines = _records(data)
    if not lines:
        raise ValueError(label + " table is empty")
    header = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
    if any(header.count(column) != 1 for column in unique_columns):
        raise ValueError(label + " table is missing or has duplicate required columns")
    rows = []
    for line in lines[1:]:
        cells = line.rstrip(b"\r\n").split(b"\t")
        if len(cells) != len(header):
            raise ValueError("Malformed " + label + " row")
        rows.append(dict(zip(header, (cell.decode("cp1252") for cell in cells))))
    return header, rows


def _locked(method):
    @wraps(method)
    def run(self, *args, **kwargs):
        with self._mutation():
            return method(self, *args, **kwargs)
    return run


class Store:
    """Root-scoped store. Methods return plain JSON-compatible dictionaries.

    ``table`` reads a workspace's unpublished state, an immutable revision, or
    baseline (default). It returns a single row when key is supplied, otherwise
    the table's columns and row count, not the potentially huge table bytes.
    ``inspect(name)`` exposes both published ``head`` and CAS ``workspace_revision``.
    """

    def __init__(self, root: Path):
        self.root = Path(root).expanduser().absolute()

    @contextmanager
    def _mutation(self):
        lock_path = _safe_store_path(self, self.root / ".lock")
        with lock_path.open("a+b") as lock:
            if os.name == "nt":
                if lock.tell() == 0:
                    lock.write(b"\0")
                    lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                yield
            finally:
                if os.name == "nt":
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock, fcntl.LOCK_UN)

    def _manifest(self):
        manifest = _load_json(_safe_store_path(self, self.root / "manifest.json"))
        if manifest.get("schema_version") != 1:
            raise ValueError("Unknown baseline schema")
        ident = _identifier(manifest["baseline_id"], "base_")
        unsigned = {k: v for k, v in manifest.items()
                    if k not in ("baseline_id", "localization_source")}
        if ident != "base_" + _digest(unsigned):
            raise ValueError("Baseline manifest identity mismatch")
        files = manifest["files"]
        if not isinstance(files, list) or not files or files != sorted(files, key=lambda f: f["path"]):
            raise ValueError("Invalid baseline file listing")
        paths = set()
        for file in files:
            path = _table(file["path"])
            if path in paths:
                raise ValueError("Duplicate table path")
            paths.add(path)
            self._checked_file(self.root / "baselines" / ident / path, file)
        if "localization_source" in manifest:
            self._source_localization_bytes(manifest)
        return manifest

    def _checked_file(self, path, record):
        path = _safe_store_path(self, path)
        data = path.read_bytes()
        if len(data) != record["bytes"] or _hash(data) != record["sha256"]:
            raise ValueError("Immutable table identity mismatch: " + str(path))
        return data

    def _source_localization_bytes(self, baseline):
        source = baseline.get("localization_source")
        fields = {"id", "path", "member", "source_archive", "sha256", "bytes"}
        if not isinstance(source, dict) or set(source) != fields:
            raise ValueError("Invalid localized string-table source record")
        _identifier(source["id"], "locsrc_")
        if (source["path"] != _PATCHSTRING or source["member"] != _PATCHSTRING_MEMBER or
                source["source_archive"] not in baseline["source_archives"] or
                not isinstance(source["sha256"], str) or not _HASH.fullmatch(source["sha256"]) or
                type(source["bytes"]) is not int or source["bytes"] <= 0 or
                source["id"] != _localization_source_identity(baseline["baseline_id"], source)):
            raise ValueError("Localized string-table source identity mismatch")
        data = self._checked_file(
            self.root / "baselines" / baseline["baseline_id"] / "resources" / "patchstring.tbl",
            source)
        mod_strings._parse(data)
        return data

    def initialize(self, tables: dict[str, bytes], source_archives: list[dict], *,
                   localization_table: bytes | None = None) -> dict:
        """Atomically install one immutable baseline; an existing root is refused."""
        if self.root.exists() or self.root.is_symlink():
            raise ValueError("Store already exists; baseline cannot be replaced")
        if not isinstance(tables, dict) or not tables:
            raise ValueError("At least one source table is required")
        if not isinstance(source_archives, list) or not source_archives:
            raise ValueError("Authenticated source archive metadata required")
        for archive in source_archives:
            if (not isinstance(archive, dict) or set(archive) != {"path", "sha256", "bytes"}
                    or not isinstance(archive["path"], str) or not archive["path"]
                    or not isinstance(archive["sha256"], str) or not _HASH.fullmatch(archive["sha256"])
                    or type(archive["bytes"]) is not int or archive["bytes"] <= 0):
                raise ValueError("Invalid source archive metadata")
        if localization_table is not None:
            if not isinstance(localization_table, bytes) or len(source_archives) != 1:
                raise ValueError("English localization requires one authenticated source archive")
            mod_strings._parse(localization_table)
        files = []
        for path, data in tables.items():
            _table(path)
            if not isinstance(data, bytes) or not data:
                raise ValueError("Source table must contain bytes")
            files.append({"path": path, "sha256": _hash(data), "bytes": len(data)})
        files.sort(key=lambda f: f["path"])
        unsigned = {"schema_version": 1, "source_archives": source_archives, "files": files}
        ident = "base_" + _digest(unsigned)
        manifest = {**unsigned, "baseline_id": ident}
        source = None
        if localization_table is not None:
            source = {"path": _PATCHSTRING, "member": _PATCHSTRING_MEMBER,
                      "source_archive": source_archives[0],
                      "sha256": _hash(localization_table), "bytes": len(localization_table)}
            source["id"] = _localization_source_identity(ident, source)
            manifest["localization_source"] = source
        self.root.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".mod-data-", dir=self.root.parent))
        try:
            for file in files:
                destination = staging / "baselines" / ident / file["path"]
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(tables[file["path"]])
            if source is not None:
                destination = staging / "baselines" / ident / "resources" / "patchstring.tbl"
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(localization_table)
            (staging / "workspaces").mkdir()
            (staging / "revisions").mkdir()
            _atomic_json(staging / "manifest.json", manifest)
            staging.rename(self.root)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        return manifest


    @staticmethod
    def _validate_legacy_localized_operations(unique_items, skills):
        try:
            normalized_items = [_legacy_unique_item_spec(item) for item in unique_items]
            normalized_skills = [_legacy_skill_spec(skill) for skill in skills]
        except (TypeError, ValueError) as error:
            raise ValueError("Legacy revision operation format is invalid") from error
        if normalized_items != unique_items or normalized_skills != skills:
            raise ValueError("Legacy revision operation format is invalid")

    @staticmethod
    def _revision_localization_id(manifest, baseline, unique_items, skills, monsters, row_templates):
        source = baseline.get("localization_source")
        source_id = manifest.get("localization_source_id")
        if not all(isinstance(entries, list) for entries in (unique_items, skills, monsters)):
            raise ValueError("Invalid revision localized operations")
        misc_items = any(op["table"] == "data/global/excel/misc.txt" for op in row_templates)
        if not unique_items and not skills and not monsters and not misc_items:
            if source_id is not None:
                raise ValueError("Unneeded localized string-table resource")
            return None
        if source_id is None:
            if monsters or misc_items:
                raise ValueError("Misc and monster revisions require authenticated English localization")
            Store._validate_legacy_localized_operations(unique_items, skills)
            return None
        if source is None or source_id != source["id"]:
            raise ValueError("Revision localized string-table source mismatch")
        return source_id

    @staticmethod
    def _revision_identity(manifest, localization_source_id):
        identity = {key: manifest[key] for key in ("baseline_id", "files", "changes")}
        for field in ("recipes", "unique_items", "skills", "monsters", "visual_assets",
                      "row_templates"):
            if field in manifest:
                identity[field] = manifest[field]
        if localization_source_id is not None:
            identity["localization_source_id"] = localization_source_id
        return identity

    def _checked_revision_inventory(self, path, manifest, baseline, localization_source_id):
        monsters = manifest.get("monsters", [])
        visual_assets = manifest.get("visual_assets", [])
        expected_paths = [file["path"] for file in baseline["files"]]
        if localization_source_id is not None:
            expected_paths.append(_PATCHSTRING)
        expected_paths.extend(record["path"] for monster in monsters
                              for record in monster.get("asset_files", []))
        expected_paths.extend(record["path"] for record in visual_assets)
        if [file["path"] for file in manifest["files"]] != sorted(expected_paths):
            raise ValueError("Revision file inventory mismatch")
        asset_paths = {record["path"] for monster in monsters
                       for record in monster.get("asset_files", [])}
        asset_paths.update(record["path"] for record in visual_assets)
        for file in manifest["files"]:
            if file["path"] == _PATCHSTRING:
                if not localization_source_id:
                    raise ValueError("Unexpected localized string-table resource")
            elif file["path"] not in asset_paths:
                _table(file["path"])
            self._checked_file(path / file["path"], file)

    def _revision(self, revision, baseline):
        _identifier(revision, "mod_")
        path = self.root / "revisions" / revision
        manifest = _load_json(_safe_store_path(self, path / "manifest.json"))
        recipes = manifest.get("recipes", [])
        unique_items = manifest.get("unique_items", [])
        skills = manifest.get("skills", [])
        monsters = self._validated_monster_definitions(manifest.get("monsters", []))
        visual_assets = manifest.get("visual_assets", [])
        if visual_assets:
            visual_assets = _validated_visual_asset_records(visual_assets)

        localization_source_id = self._revision_localization_id(
            manifest, baseline, unique_items, skills, monsters, manifest.get("row_templates", []))
        if (manifest.get("schema_version") != 1 or
                any(field in manifest and not manifest[field]
                    for field in ("recipes", "unique_items", "skills", "monsters", "visual_assets")) or
                manifest.get("revision") != revision or
                manifest.get("baseline_id") != baseline["baseline_id"]):
            raise ValueError("Revision metadata mismatch")
        if manifest.get("source_archives") != baseline["source_archives"]:
            raise ValueError("Revision provenance mismatch")
        if revision != "mod_" + _digest(
                self._revision_identity(manifest, localization_source_id)):
            raise ValueError("Revision identity mismatch")
        self._checked_revision_inventory(path, manifest, baseline, localization_source_id)
        self._validate_changes(manifest["changes"], baseline)
        row_templates = _validated_row_templates(manifest.get("row_templates", []), baseline)
        if self._render(
                baseline, manifest["changes"], recipes, unique_items, skills, monsters,
                visual_assets=visual_assets, row_templates=row_templates,
                legacy=localization_source_id is None) != self._revision_bytes(path, manifest):
            raise ValueError("Revision bytes differ from baseline and authored operations")
        return manifest

    def _revision_bytes(self, path, manifest):
        return {f["path"]: self._checked_file(path / f["path"], f) for f in manifest["files"]}

    def _baseline_bytes(self, baseline):
        return {f["path"]: self._checked_file(self.root / "baselines" / baseline["baseline_id"] / f["path"], f)
                for f in baseline["files"]}

    @staticmethod
    def _validate_changes(changes, baseline):
        if not isinstance(changes, list) or any(
            not isinstance(c, dict) or set(c) != {"table", "key_column", "key", "column", "expected", "value"}
            for c in changes
        ):
            raise ValueError("Invalid change")
        if changes != sorted(changes, key=lambda c: (c["table"], c["key_column"], c["key"], c["column"])):
            raise ValueError("Unsorted changes")
        inventory = {f["path"] for f in baseline["files"]}
        seen = set()
        for change in changes:
            if not isinstance(change, dict) or set(change) != {"table", "key_column", "key", "column", "expected", "value"}:
                raise ValueError("Invalid change")
            if _table(change["table"]) not in inventory:
                raise ValueError("Unknown source table")
            legacy_table = (change["table"] == _LEGACY_TREASURECLASS and
                            change["key_column"] == _LEGACY_TREASURE_KEY)
            legacy_item = (
                legacy_table and
                re.fullmatch(r"Item(10|[1-9])", change["column"]) and
                bool(_ITEM_CODE.fullmatch(change["expected"])) and
                change["value"] == "")
            legacy_probability = (
                legacy_table and
                re.fullmatch(r"Prob(10|[1-9])", change["column"]) and
                change["expected"].isdigit() and
                int(change["expected"]) > 0 and change["value"] == "0")
            legacy_no_drop = (
                legacy_table and change["column"] == "NoDrop" and
                (change["expected"] == "" or change["expected"].isdigit()) and
                change["value"].isdigit() and
                int(change["value"]) > int(change["expected"] or "0"))
            legacy_portal = (
                legacy_table and
                re.fullmatch(r"Item(10|[1-9])", change["column"]) and
                change["expected"] == "tsc" and change["value"] == "isc")
            if ((change["key_column"] == _LEGACY_TREASURE_KEY or
                 change["table"] == _LEGACY_TREASURECLASS) and
                    not (legacy_item or legacy_probability or
                         legacy_no_drop or legacy_portal)):
                raise ValueError("Legacy treasure edits must target supported drop cells")
            for field in ("key_column", "column", "key", "expected", "value"):
                _cell(change[field])
            if not change["key_column"] or not change["column"] or change["column"] == change["key_column"] or change["column"].lower() in _IDENTITY:
                raise ValueError("Identity or key column cannot be modified")
            target = tuple(change[f] for f in ("table", "key_column", "key", "column"))
            if target in seen:
                raise ValueError("Duplicate target edit")
            seen.add(target)

    def _item_codes(self, baseline):
        inventory = {file["path"] for file in baseline["files"]}
        codes = set()
        for filename in _BASE_ITEM_TABLES:
            path = "data/global/excel/" + filename
            if path not in inventory:
                continue
            lines = _records(self._checked_file(
                self.root / "baselines" / baseline["baseline_id"] / path,
                next(file for file in baseline["files"] if file["path"] == path)))
            header = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
            if header.count("code") != 1:
                raise ValueError("Base item table must have one code column: " + filename)
            index = header.index("code")
            for line in lines[1:]:
                cells = line.rstrip(b"\r\n").split(b"\t")
                if len(cells) != len(header):
                    raise ValueError("Malformed base item row: " + filename)
                code = cells[index].decode("cp1252")
                if code:
                    if code in codes:
                        raise ValueError("Ambiguous base item code: " + code)
                    codes.add(code)
        if not codes:
            raise ValueError("Authenticated base item tables are unavailable")
        return codes

    @staticmethod
    def _base_item_rows(tables):
        items = {}
        for filename in _BASE_ITEM_TABLES:
            path = "data/global/excel/" + filename
            if path not in tables:
                continue
            header, rows = _tabular_rows(tables[path], filename, ("code",))
            for row in rows:
                code = row["code"]
                if code:
                    if code in items:
                        raise ValueError("Ambiguous base item code: " + code)
                    items[code] = (row, header)
        if not items:
            raise ValueError("Authenticated base item tables are unavailable")
        return items

    def _render_recipes(self, data, recipes, baseline):
        if not isinstance(recipes, list) or len(recipes) > 64:
            raise ValueError("Workspace supports at most 64 cube recipes")
        normalized = [_recipe_spec(recipe) for recipe in recipes]
        if (normalized != recipes or
                len({r["description"].casefold() for r in recipes}) != len(recipes) or
                len({_recipe_identity(r) for r in recipes}) != len(recipes)):
            raise ValueError("Invalid or duplicate authored recipe")
        codes = self._item_codes(baseline)
        for recipe in recipes:
            if any(item["code"] not in codes for item in recipe["inputs"]) or recipe["output"] not in codes:
                raise ValueError("Recipe references an unknown base item code")

        lines = _records(data)
        header = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
        columns = ("description", "enabled", "version", "numinputs", "output", "*eol",
                   *(f"input {index}" for index in range(1, 8)))
        if any(header.count(column) != 1 for column in columns):
            raise ValueError("cubemain.txt is missing a unique recipe column")
        indexes = {column: header.index(column) for column in columns}
        descriptions, signatures = set(), set()
        for line in lines[1:]:
            cells = line.rstrip(b"\r\n").split(b"\t")
            if len(cells) != len(header):
                raise ValueError("Malformed cubemain.txt row")
            descriptions.add(cells[indexes["description"]].decode("cp1252").casefold())
            try:
                count = int(cells[indexes["numinputs"]])
                if not 1 <= count <= 7:
                    continue
                items = []
                for index in range(1, count + 1):
                    value = cells[indexes[f"input {index}"]].decode("cp1252")
                    if value.startswith('"') and value.endswith('"'):
                        value = value[1:-1]
                    match = re.fullmatch(r"([a-z0-9]{3,4})(?:,qty=([1-9][0-9]{0,2}))?", value)
                    if not match:
                        raise ValueError
                    items.append((match.group(1), int(match.group(2) or "1")))
                signatures.add(tuple(sorted(items)))
            except (ValueError, IndexError):
                continue

        ending = b"\r\n" if lines[0].endswith(b"\r\n") else b"\n" if lines[0].endswith(b"\n") else b"\r"
        additions = []
        for recipe in recipes:
            signature = _recipe_identity(recipe)
            if recipe["description"].casefold() in descriptions:
                raise ValueError("Cube recipe description already exists")
            if signature in signatures:
                raise ValueError("Cube recipe input signature already exists")
            row = [b""] * len(header)
            row[indexes["description"]] = _cell(recipe["description"])
            row[indexes["enabled"]] = b"1"
            row[indexes["version"]] = b"100"
            row[indexes["numinputs"]] = str(len(recipe["inputs"])).encode("ascii")
            row[indexes["output"]] = _cell(recipe["output"])
            row[indexes["*eol"]] = b"0"
            for index, item in enumerate(recipe["inputs"], 1):
                row[indexes[f"input {index}"]] = _cell(
                    f'"{item["code"]},qty={item["quantity"]}"')
            additions.append(b"\t".join(row))
            descriptions.add(recipe["description"].casefold())
            signatures.add(signature)
        if not additions:
            return data
        separator = b"" if data.endswith((b"\r", b"\n")) else ending
        return data + separator + ending.join(additions) + ending

    @staticmethod
    def _validate_unique_stat(tables, item):
        properties_path = _PROPERTIES
        stats_path = _ITEMSTATCOST
        if properties_path not in tables or stats_path not in tables:
            raise ValueError("Authenticated property and itemstatcost tables are required")
        property_header, properties = _tabular_rows(
            tables[properties_path], "properties.txt", ("code", "func1", "stat1"))
        if any(property_header.count(f"{field}{index}") > 1
               for index in range(2, 8) for field in ("stat", "func")):
            raise ValueError("properties.txt has ambiguous stat columns")
        matches = [row for row in properties if row.get("code") == item["stat"]]
        stat_name = _UNIQUE_STATS[item["stat"]]
        if len(matches) != 1:
            raise ValueError("Unique stat property must exist exactly once: " + item["stat"])
        prop = matches[0]
        if (prop.get("func1") != "1" or prop.get("stat1") != stat_name or
                any(prop.get(f"stat{index}") or prop.get(f"func{index}") for index in range(2, 8))):
            raise ValueError("Unique stat property is not a direct single-stat mapping: " + item["stat"])
        _, stats = _tabular_rows(tables[stats_path], "itemstatcost.txt", ("Stat",))
        if sum(row.get("Stat") == stat_name for row in stats) != 1:
            raise ValueError("Item stat must exist exactly once: " + stat_name)

    def _render_unique_items(self, data, unique_items, tables, *, legacy=False):
        if not isinstance(unique_items, list) or len(unique_items) > 16:
            raise ValueError("Workspace supports at most 16 new unique items")
        validator = _legacy_unique_item_spec if legacy else _unique_item_spec
        normalized = [validator(item) for item in unique_items]
        if (normalized != unique_items or
                len({item["name"].casefold() for item in unique_items}) != len(unique_items)):
            raise ValueError("Invalid or duplicate authored unique item")
        if not unique_items:
            return data
        header, existing = _tabular_rows(data, "uniqueitems.txt")
        columns = ("index", "version", "enabled", "rarity", "lvl", "lvl req", "code",
                   "*type", "cost mult", "cost add", "invfile", "prop1", "par1",
                   "min1", "max1", "*eol")
        if any(header.count(column) != 1 for column in columns):
            raise ValueError("uniqueitems.txt is missing a unique required column")
        indexes = {column: header.index(column) for column in columns}
        names = {row["index"].casefold() for row in existing if row["index"]}
        bases = self._base_item_rows(tables)
        ending = b"\r\n" if data.endswith(b"\r\n") else b"\n" if data.endswith(b"\n") else b"\r"
        additions = []
        for item_index, item in enumerate(unique_items, 1):
            index = item["name"] if legacy else _unique_string_key(item_index)
            if index.casefold() in names:
                label = "Unique item name" if legacy else "Unique item localization key"
                raise ValueError(label + " already exists: " + index)
            base_record = bases.get(item["base_code"])
            if not base_record:
                raise ValueError("Unique item requires an existing spawnable base code")
            base, base_header = base_record
            if (any(base_header.count(column) != 1 for column in ("name", "spawnable", "invfile")) or
                    base_header.count("uniqueinvfile") > 1):
                raise ValueError("Unique item base has missing or ambiguous required columns")
            if base.get("spawnable") != "1" or not base.get("name"):
                raise ValueError("Unique item requires an existing spawnable base code")
            invfile = base.get("uniqueinvfile") or base.get("invfile")
            if not invfile:
                raise ValueError("Unique item base has no usable inventory graphic")
            self._validate_unique_stat(tables, item)
            row = [b""] * len(header)
            values = {
                "index": index, "version": "0", "enabled": "1", "rarity": "1",
                "lvl": str(item["level"]), "lvl req": str(item["required_level"]),
                "code": item["base_code"], "*type": base["name"], "cost mult": "5",
                "cost add": "5000", "invfile": invfile, "prop1": item["stat"],
                "min1": str(item["value"]), "max1": str(item["value"]), "*eol": "0",
            }
            for column, value in values.items():
                row[indexes[column]] = _cell(value)
            additions.append(b"\t".join(row))
            names.add(index.casefold())
        separator = b"" if data.endswith((b"\r", b"\n")) else ending
        return data + separator + ending.join(additions) + ending

    @staticmethod
    def _append_tsv_rows(data, header, rows):
        if not rows:
            return data
        ending = b"\r\n" if data.endswith(b"\r\n") else b"\n" if data.endswith(b"\n") else b"\r"
        additions = [b"\t".join(_cell(row[column]) for column in header) for row in rows]
        separator = b"" if data.endswith((b"\r", b"\n")) else ending
        return data + separator + ending.join(additions) + ending

    @staticmethod
    def _sorceress_fire_tree_slot(description):
        page_text = description["SkillPage"]
        if page_text in ("", "0"):
            return None
        try:
            page = int(page_text)
        except ValueError as error:
            raise ValueError("Sorceress skill page is not numeric") from error
        if page != 1:
            return None
        try:
            row = int(description["SkillRow"])
            column = int(description["SkillColumn"])
        except ValueError as error:
            raise ValueError("Sorceress Fire-tree slot is not numeric") from error
        if not 1 <= row <= 6 or not 1 <= column <= 3:
            raise ValueError("Sorceress Fire-tree slot is outside the supported layout")
        return row, column

    @staticmethod
    def _next_sorceress_fire_slot(skill_rows, descriptions):
        occupied = set()
        for skill in skill_rows:
            if skill["charclass"] != "sor" or not skill["skilldesc"]:
                continue
            description = descriptions.get(skill["skilldesc"].casefold())
            if description is None:
                raise ValueError("Sorceress skill references a missing skill description")
            slot = Store._sorceress_fire_tree_slot(description)
            if slot is not None:
                occupied.add(slot)
        for row in range(1, 7):
            for column in range(1, 4):
                if (row, column) not in occupied:
                    return row, column
        raise ValueError("Sorceress Fire tree has no free skill slot")

    @staticmethod
    def _sorceress_cold_hydra_slot(skill_rows, descriptions):
        for skill in skill_rows:
            if skill["charclass"] != "sor" or not skill["skilldesc"]:
                continue
            description = descriptions.get(skill["skilldesc"].casefold())
            if description is None:
                raise ValueError("Sorceress skill references a missing skill description")
            if (description["SkillPage"], description["SkillRow"],
                    description["SkillColumn"]) == ("3", "6", "3"):
                raise ValueError("Sorceress Cold-tree Hydra slot is already occupied")
        return 6, 3

    @staticmethod
    def _sorceress_hydra_palette_slots(skill_rows, descriptions):
        occupied = set()
        for skill in skill_rows:
            if skill["charclass"] != "sor" or not skill["skilldesc"]:
                continue
            description = descriptions.get(skill["skilldesc"].casefold())
            if description is None:
                raise ValueError("Sorceress skill references a missing skill description")
            occupied.add((description["SkillPage"], description["SkillRow"],
                          description["SkillColumn"]))
        if any((str(page), str(row), str(column)) in occupied
               for page, row, column in _HYDRA_PALETTE_TREE_SLOTS):
            raise ValueError("Sorceress Lightning-tree Hydra palette slot is already occupied")
        return _HYDRA_PALETTE_TREE_SLOTS

    @staticmethod
    def _is_hydra_palette(skills):
        levels = {skill.get("monster_trans_level", _COLD_HYDRA_TRANSLEVEL) for skill in skills}
        return (len(skills) == len(_HYDRA_PALETTE_TREE_SLOTS) and
                all(skill.get("template") == "cold_hydra" for skill in skills) and
                levels == set(range(len(_HYDRA_PALETTE_TREE_SLOTS))))

    @staticmethod
    def _skill_indexes(skill_rows, desc_rows):
        ids, names, descriptions = set(), set(), {}
        for row in skill_rows:
            try:
                skill_id = int(row["Id"])
            except ValueError as error:
                raise ValueError("Skill IDs must be numeric") from error
            if skill_id < 0 or skill_id in ids:
                raise ValueError("Skill IDs must be unique nonnegative integers")
            ids.add(skill_id)
            if row["skill"]:
                names.add(row["skill"].casefold())
        for row in desc_rows:
            key = row["skilldesc"].casefold()
            if key and key in descriptions:
                raise ValueError("Skill description keys must be unique")
            if key:
                descriptions[key] = row
        return ids, names, descriptions

    @staticmethod
    def _fire_bolt_template(skill_rows, descriptions, missile_rows):
        template_rows = [row for row in skill_rows if row["Id"] == "36"]
        if len(template_rows) != 1:
            raise ValueError("Authenticated Fire Bolt skill ID 36 is missing or ambiguous")
        template = template_rows[0]
        if (template["skill"] != "Fire Bolt" or template["charclass"] != "sor" or
                template["skilldesc"].casefold() != "fire bolt" or template["InGame"] != "1" or
                template["srvmissile"].casefold() != "firebolt" or
                template["cltmissile"].casefold() != "firebolt" or
                any(template[column] for column in ("reqskill1", "reqskill2", "reqskill3"))):
            raise ValueError("Fire Bolt no longer matches the supported Sorceress missile template")
        template_desc = descriptions.get(template["skilldesc"].casefold())
        if template_desc is None or template_desc["SkillPage"] != "1":
            raise ValueError("Fire Bolt skill-tree description is missing or incompatible")
        if sum(row["Missile"].casefold() == "firebolt" for row in missile_rows) != 1:
            raise ValueError("Fire Bolt missile reference is missing or ambiguous")
        return template, template_desc

    @staticmethod
    def _exact_source_row(rows, column, value, label):
        matches = [row for row in rows if row.get(column) == value]
        if len(matches) != 1:
            raise ValueError(f"Authenticated {label} is missing or ambiguous")
        return matches[0]

    @staticmethod
    def _cold_hydra_source_tables(tables):
        required_tables = (_SKILLS, _SKILLDESC, _MISSILES, _MONSTATS, _MONSTATS2)
        if any(path not in tables for path in required_tables):
            raise ValueError("Authenticated Hydra skill, monster, or asset-reference table is unavailable")
        damage_fields = ("EType", "EMin", "EMinLev1", "EMinLev2", "EMinLev3", "EMinLev4",
                         "EMinLev5", "EMax", "EMaxLev1", "EMaxLev2", "EMaxLev3",
                         "EMaxLev4", "EMaxLev5", "EDmgSymPerCalc", "ELen", "ELevLen1",
                         "ELevLen2", "ELevLen3", "ELenSymPerCalc")
        skill_columns = (
            "skill", "Id", "charclass", "skilldesc", "srvstfunc", "srvdofunc",
            "passivestat1", "passivecalc1", "summon", "pettype", "petmax", "summode",
            "sumskill1", "sumsk1calc", "sumskill2", "sumsk2calc", "sumskill3",
            "sumsk3calc", "stsound", "castoverlay", "ItemCastSound", "ItemCastOverlay",
            "reqlevel", "reqskill1", "reqskill2", "reqskill3", "Param1", "Param7",
            "Param8", "srvmissile", "cltmissile", *damage_fields)
        skill_header, skill_rows = _tabular_rows(tables[_SKILLS], "skills.txt", skill_columns)
        desc_header, desc_rows = _tabular_rows(
            tables[_SKILLDESC], "skilldesc.txt",
            ("skilldesc", "SkillPage", "SkillRow", "SkillColumn", "IconCel",
             "str name", "str short", "str long", "str alt", "dsc3textb1"))
        _, missile_rows = _tabular_rows(
            tables[_MISSILES], "missiles.txt",
            ("Missile", "CelFile", "EType", "Red", "Green", "Blue"))
        monster_header, monster_rows = _tabular_rows(
            tables[_MONSTATS], "monstats.txt",
            ("Id", "hcIdx", "BaseId", "NextInClass", "TransLvl", "NameStr",
             "MonStatsEx", "AI", "Code", "Skill1", "Sk1mode", "Sk1lvl"))
        _, monster2_rows = _tabular_rows(tables[_MONSTATS2], "monstats2.txt", ("Id",))
        return {
            "damage_fields": damage_fields,
            "skill_header": skill_header,
            "skill_rows": skill_rows,
            "desc_header": desc_header,
            "desc_rows": desc_rows,
            "missile_rows": missile_rows,
            "monster_header": monster_header,
            "monster_rows": monster_rows,
            "monster2_rows": monster2_rows,
        }

    @staticmethod
    def _validate_hydra_skill_source(hydra, hydra_desc):
        if (hydra["Id"] != "62" or hydra["charclass"] != "sor" or
                hydra["skilldesc"].casefold() != "hydra" or
                hydra["srvstfunc"] != "14" or hydra["srvdofunc"] != "144" or
                hydra["passivestat1"] != "passive_fire_mastery" or
                hydra["passivecalc1"] != "stat('passive_fire_mastery'.accr)" or
                hydra["summon"] != "hydra1" or hydra["pettype"] != "hydra" or
                hydra["summode"] != "S2" or hydra["sumskill1"] != "HydraFireball" or
                hydra["reqlevel"] != "30" or hydra_desc["SkillPage"] != "1" or
                (hydra_desc["SkillRow"], hydra_desc["SkillColumn"]) != ("6", "3")):
            raise ValueError("Authenticated Hydra no longer matches the supported Sorceress template")

    @staticmethod
    def _validate_glacial_spike_skill_source(spike, spike_desc):
        if (spike["Id"] != "55" or spike["charclass"] != "sor" or
                spike["skilldesc"].casefold() != "glacial spike" or
                spike["srvmissile"] != "glacialspike" or
                spike["cltmissile"] != "glacialspike" or spike["EType"] != "cold" or
                spike_desc["SkillPage"] != "3" or
                (spike_desc["SkillRow"], spike_desc["SkillColumn"]) != ("4", "2")):
            raise ValueError("Authenticated Glacial Spike no longer matches the supported Cold-tree missile")

    @staticmethod
    def _validate_cold_synergies(cold_mastery, ice_bolt, ice_blast):
        if (cold_mastery["Id"] != "65" or cold_mastery["charclass"] != "sor" or
                cold_mastery["passivestat1"] != "passive_cold_pierce" or
                ice_bolt["charclass"] != "sor" or ice_blast["charclass"] != "sor"):
            raise ValueError("Authenticated Cold Mastery or Glacial Spike synergies are incompatible")

    @staticmethod
    def _validate_hydra_fireball_source(fireball, fireball_missile):
        if (fireball["Id"] != "586" or fireball["srvmissile"] != "hydrafireball" or
                fireball["cltmissile"] != "hydrafireball" or
                fireball_missile["CelFile"] != "Fireball" or fireball_missile["EType"] != "fire"):
            raise ValueError("Authenticated HydraFireball attack is missing or incompatible")

    @staticmethod
    def _validate_glacial_spike_missile(missile):
        if (missile["CelFile"] != "GlacialSpike" or missile["EType"] != "frze" or
                (missile["Red"], missile["Green"], missile["Blue"]) != ("81", "81", "255")):
            raise ValueError("Authenticated Glacial Spike missile is missing or incompatible")

    @staticmethod
    def _validate_hydra_monster_source(monster):
        if (monster["BaseId"] != "hydra1" or monster["MonStatsEx"] != "hydra1" or
                monster["AI"] != "Hydra" or monster["Code"] != "HX" or
                monster["Skill1"] != "HydraFireball" or monster["TransLvl"] != "0" or
                not monster["hcIdx"].isdigit()):
            raise ValueError("Authenticated Hydra monster no longer matches the supported model")

    def _cold_hydra_sources(self, tables):
        source = self._cold_hydra_source_tables(tables)
        skill_rows, desc_rows = source["skill_rows"], source["desc_rows"]
        monster_rows, monster2_rows = source["monster_rows"], source["monster2_rows"]
        missile_rows = source["missile_rows"]
        source.update({
            "hydra": self._exact_source_row(skill_rows, "skill", "Hydra", "Hydra skill"),
            "hydra_desc": self._exact_source_row(
                desc_rows, "skilldesc", "hydra", "Hydra skill description"),
            "spike": self._exact_source_row(skill_rows, "skill", "Glacial Spike",
                                            "Glacial Spike skill"),
            "spike_desc": self._exact_source_row(
                desc_rows, "skilldesc", "glacial spike", "Glacial Spike skill description"),
            "fireball": self._exact_source_row(skill_rows, "skill", "HydraFireball",
                                               "HydraFireball attack skill"),
            "cold_mastery": self._exact_source_row(skill_rows, "skill", "Cold Mastery",
                                                   "Cold Mastery skill"),
            "ice_bolt": self._exact_source_row(skill_rows, "skill", "Ice Bolt", "Ice Bolt synergy"),
            "ice_blast": self._exact_source_row(skill_rows, "skill", "Ice Blast", "Ice Blast synergy"),
            "hydra_monster": self._exact_source_row(monster_rows, "Id", "hydra1", "Hydra monster"),
            "spike_missile": self._exact_source_row(
                missile_rows, "Missile", "glacialspike", "Glacial Spike missile"),
            "fireball_missile": self._exact_source_row(
                missile_rows, "Missile", "hydrafireball", "Hydra fireball missile"),
        })
        self._exact_source_row(monster2_rows, "Id", "hydra1", "Hydra animation definition")
        self._validate_hydra_skill_source(source["hydra"], source["hydra_desc"])
        self._validate_glacial_spike_skill_source(source["spike"], source["spike_desc"])
        self._validate_cold_synergies(
            source["cold_mastery"], source["ice_bolt"], source["ice_blast"])
        self._validate_hydra_fireball_source(source["fireball"], source["fireball_missile"])
        self._validate_glacial_spike_missile(source["spike_missile"])
        self._validate_hydra_monster_source(source["hydra_monster"])
        return source

    def _cold_hydra_rows(self, skill, skill_id, string_keys, source,
                         skill_rows, descriptions, names, ids, tree_slot=None):
        if tree_slot is None:
            page = 3
            row, column = self._sorceress_cold_hydra_slot(skill_rows, descriptions)
        else:
            page, row, column = tree_slot
        attack_id = skill_id + 1
        attack_name = f"D2Lab Glacial Hydra Attack {skill_id}"
        if attack_name.casefold() in names or attack_id in ids:
            raise ValueError("Cold Hydra attack identity is already occupied")
        monster_id = f"d2labhydra{skill_id}"
        monster_rows = source["monster_rows"]
        if any(item["Id"].casefold() == monster_id.casefold() for item in monster_rows):
            raise ValueError("Cold Hydra monster identity is already occupied")
        hc_indexes = [int(item["hcIdx"]) for item in monster_rows if item["hcIdx"].isdigit()]
        hc_index = max(hc_indexes, default=-1) + 1
        if hc_index > 65535:
            raise ValueError("No safe monster class index remains for the Cold Hydra")
        hydra_skill = {
            **source["hydra"], "skill": skill["name"], "Id": str(skill_id),
            "charclass": "sor", "skilldesc": skill["name"].casefold(), "summon": monster_id,
            "passivestat1": source["cold_mastery"]["passivestat1"],
            "passivecalc1": source["hydra"]["passivecalc1"].replace(
                "passive_fire_mastery", source["cold_mastery"]["passivestat1"]),
            "sumskill1": attack_name, "sumsk1calc": "lvl",
            "sumskill2": "Ice Bolt", "sumsk2calc": "skill('Ice Bolt'.blvl)",
            "sumskill3": "Ice Blast", "sumsk3calc": "skill('Ice Blast'.blvl)",
            "stsound": "sorceress_cast_cold", "castoverlay": "ice_cast_2",
            "ItemCastSound": "sorceress_cast_cold", "ItemCastOverlay": "ice_cast_2",
            "reqskill1": "Glacial Spike", "reqskill2": "", "reqskill3": "",
            "Param7": source["spike"]["Param7"],
            "Param8": source["spike"]["Param8"],
        }
        for field in source["damage_fields"]:
            hydra_skill[field] = source["spike"][field]
        description = {
            **source["spike_desc"], "skilldesc": skill["name"].casefold(),
            "SkillPage": str(page), "SkillRow": str(row), "SkillColumn": str(column),
            "IconCel": source["hydra_desc"]["IconCel"],
            "str name": string_keys["name"], "str short": string_keys["short"],
            "str long": string_keys["long"], "str alt": string_keys["alt"],
        }
        for field, value in description.items():
            if value == "skillname55":
                description[field] = string_keys["name"]
        attack = {
            **source["fireball"], "skill": attack_name, "Id": str(attack_id),
            "srvmissile": "glacialspike", "cltmissile": "glacialspike",
        }
        hydra_monster = {
            **source["hydra_monster"], "Id": monster_id,
            "hcIdx": str(hc_index), "BaseId": "hydra1", "NextInClass": "",
            "TransLvl": str(skill.get("monster_trans_level", _COLD_HYDRA_TRANSLEVEL)),
            "AI": "Hydra", "Skill1": attack_name, "Sk1mode": "A1", "Sk1lvl": "1",
        }
        return hydra_skill, attack, description, hydra_monster, attack_name.casefold()

    def _fire_bolt_rows(self, skill, skill_id, string_keys, fire_template,
                        fire_template_desc, skill_rows, descriptions):
        name = skill["name"]
        key = name.casefold()
        row, column = self._next_sorceress_fire_slot(skill_rows, descriptions)
        skill_row = {**fire_template, "skill": name, "Id": str(skill_id),
                     "charclass": "sor", "skilldesc": key}
        desc_row = {**fire_template_desc, "skilldesc": key, "SkillPage": "1",
                    "SkillRow": str(row), "SkillColumn": str(column),
                    "str name": string_keys["name"], "str short": string_keys["short"],
                    "str long": string_keys["long"], "str alt": string_keys["alt"]}
        if desc_row.get("dsc3textb1") == "skillname36":
            desc_row["dsc3textb1"] = string_keys["name"]
        return skill_row, desc_row

    def _render_legacy_skills(self, tables, skills):
        if not isinstance(skills, list) or len(skills) > 8:
            raise ValueError("Workspace supports at most 8 Fire Bolt-based Sorceress skills")
        normalized = [_legacy_skill_spec(skill) for skill in skills]
        if (normalized != skills or
                len({skill["name"].casefold() for skill in skills}) != len(skills)):
            raise ValueError("Invalid or duplicate authored skill")
        if not skills:
            return tables[_SKILLS], tables[_SKILLDESC], None
        if any(path not in tables for path in (_SKILLS, _SKILLDESC, _MISSILES)):
            raise ValueError("Authenticated skill or missile table is unavailable")

        skill_header, skill_rows = _tabular_rows(
            tables[_SKILLS], "skills.txt",
            ("skill", "Id", "charclass", "skilldesc", "srvstfunc", "srvdofunc",
             "srvmissile", "cltmissile", "InGame", "reqskill1", "reqskill2", "reqskill3"))
        desc_header, desc_rows = _tabular_rows(
            tables[_SKILLDESC], "skilldesc.txt",
            ("skilldesc", "SkillPage", "SkillRow", "SkillColumn", "str name"))
        _, missile_rows = _tabular_rows(tables[_MISSILES], "missiles.txt", ("Missile",))
        ids, names, descriptions = self._skill_indexes(skill_rows, desc_rows)
        template, template_desc = self._fire_bolt_template(
            skill_rows, descriptions, missile_rows)

        skill_additions, desc_additions = [], []
        next_id = max(ids) + 1
        for skill in skills:
            name = skill["name"]
            key = name.casefold()
            if key in names or key in descriptions:
                raise ValueError("New skill name or description key already exists: " + name)
            row, column = self._next_sorceress_fire_slot(skill_rows, descriptions)
            skill_row = {**template, "skill": name, "Id": str(next_id),
                         "charclass": "sor", "skilldesc": key}
            desc_row = {**template_desc, "skilldesc": key, "SkillPage": "1",
                        "SkillRow": str(row), "SkillColumn": str(column), "str name": name}
            skill_additions.append(skill_row)
            desc_additions.append(desc_row)
            skill_rows.append(skill_row)
            descriptions[key] = desc_row
            names.add(key)
            ids.add(next_id)
            next_id += 1

        return (self._append_tsv_rows(tables[_SKILLS], skill_header, skill_additions),
                self._append_tsv_rows(tables[_SKILLDESC], desc_header, desc_additions), None)

    def _skill_render_context(self, tables, skills):
        if any(path not in tables for path in (_SKILLS, _SKILLDESC, _MISSILES)):
            raise ValueError("Authenticated skill or missile table is unavailable")
        cold_skills = [skill for skill in skills if skill.get("template") == "cold_hydra"]
        palette = self._is_hydra_palette(cold_skills)
        if len(cold_skills) > 1 and not palette:
            raise ValueError("Multiple Cold Hydra skills require the complete 0–7 palette set")
        cold_source = self._cold_hydra_sources(tables) if cold_skills else None
        if cold_source:
            skill_header, skill_rows = cold_source["skill_header"], cold_source["skill_rows"]
            desc_header, desc_rows = cold_source["desc_header"], cold_source["desc_rows"]
            missile_rows = None
        else:
            skill_header, skill_rows = _tabular_rows(
                tables[_SKILLS], "skills.txt",
                ("skill", "Id", "charclass", "skilldesc", "srvstfunc", "srvdofunc",
                 "srvmissile", "cltmissile", "InGame", "reqskill1", "reqskill2", "reqskill3"))
            desc_header, desc_rows = _tabular_rows(
                tables[_SKILLDESC], "skilldesc.txt",
                ("skilldesc", "SkillPage", "SkillRow", "SkillColumn",
                 "str name", "str short", "str long", "str alt"))
            _, missile_rows = _tabular_rows(tables[_MISSILES], "missiles.txt", ("Missile",))
        ids, names, descriptions = self._skill_indexes(skill_rows, desc_rows)
        fire_template = fire_template_desc = None
        if any(skill.get("template", "fire_bolt") == "fire_bolt" for skill in skills):
            if missile_rows is None:
                _, missile_rows = _tabular_rows(tables[_MISSILES], "missiles.txt", ("Missile",))
            fire_template, fire_template_desc = self._fire_bolt_template(
                skill_rows, descriptions, missile_rows)
        palette_slots = (self._sorceress_hydra_palette_slots(skill_rows, descriptions)
                         if palette else ())
        return {
            "cold_source": cold_source, "descriptions": descriptions,
            "desc_header": desc_header, "fire_template": fire_template,
            "fire_template_desc": fire_template_desc, "ids": ids, "names": names,
            "palette": palette, "palette_slots": palette_slots,
            "skill_header": skill_header,
            "skill_rows": skill_rows,
        }

    def _render_skill_additions(self, tables, skills, context):
        skill_additions, desc_additions, monster_additions = [], [], []
        skill_rows, descriptions = context["skill_rows"], context["descriptions"]
        names, ids = context["names"], context["ids"]
        next_id = max(ids) + 1
        palette_slot_index = 0
        for skill in skills:
            name = skill["name"]
            key = name.casefold()
            if key in names or key in descriptions:
                raise ValueError("New skill name or description key already exists: " + name)
            string_keys = _skill_string_keys(next_id)
            if skill.get("template", "fire_bolt") == "cold_hydra":
                tree_slot = (context["palette_slots"][palette_slot_index]
                             if context["palette"] else None)
                hydra_skill, attack, description, hydra_monster, attack_name = (
                    self._cold_hydra_rows(
                        skill, next_id, string_keys, context["cold_source"], skill_rows,
                        descriptions, names, ids, tree_slot=tree_slot))
                skill_additions.extend((hydra_skill, attack))
                desc_additions.append(description)
                monster_additions.append(hydra_monster)
                skill_rows.extend((hydra_skill, attack))
                descriptions[key] = description
                names.update((key, attack_name))
                ids.update((next_id, next_id + 1))
                if context["palette"]:
                    context["cold_source"]["monster_rows"].append(hydra_monster)
                    palette_slot_index += 1
                next_id += 2
            else:
                skill_row, desc_row = self._fire_bolt_rows(
                    skill, next_id, string_keys, context["fire_template"],
                    context["fire_template_desc"], skill_rows, descriptions)
                skill_additions.append(skill_row)
                desc_additions.append(desc_row)
                skill_rows.append(skill_row)
                descriptions[key] = desc_row
                names.add(key)
                ids.add(next_id)
                next_id += 1
        monster_data = None
        if monster_additions:
            monster_data = self._append_tsv_rows(
                tables[_MONSTATS], context["cold_source"]["monster_header"], monster_additions)
        return (self._append_tsv_rows(tables[_SKILLS], context["skill_header"], skill_additions),
                self._append_tsv_rows(tables[_SKILLDESC], context["desc_header"], desc_additions),
                monster_data)

    def _render_skills(self, tables, skills):
        if not isinstance(skills, list) or len(skills) > 8:
            raise ValueError("Workspace supports at most 8 typed Sorceress skills")
        normalized = [_skill_spec(skill) for skill in skills]
        if (normalized != skills or
                len({skill["name"].casefold() for skill in skills}) != len(skills)):
            raise ValueError("Invalid or duplicate authored skill")
        if not skills:
            return tables[_SKILLS], tables[_SKILLDESC], None
        context = self._skill_render_context(tables, skills)
        return self._render_skill_additions(tables, skills, context)

    @staticmethod
    def _validated_monster_definitions(monsters):
        if not isinstance(monsters, list) or len(monsters) > 16:
            raise ValueError("Workspace supports at most 16 typed monsters")
        normalized = [_monster_spec(monster) for monster in monsters]
        if normalized != monsters or len({monster["token"] for monster in monsters}) != len(monsters):
            raise ValueError("Invalid or duplicate authored monster token")
        if len({monster["name"].casefold() for monster in monsters}) != len(monsters):
            raise ValueError("Duplicate authored monster name")
        paths = [record["path"] for monster in monsters
                 for record in monster.get("asset_files", [])]
        if len(paths) != len(set(paths)):
            raise ValueError("Monster animation asset paths must be unique")
        return monsters

    @staticmethod
    def _monster_row_additions(monsters, template, template2, ids, ids2, codes, next_index):
        additions, additions2 = [], []
        for monster in monsters:
            token = monster["token"]
            ident = f"d2labbrute{token.lower()}"
            if ident.casefold() in ids or ident.casefold() in ids2 or token.casefold() in codes:
                raise ValueError("Monster Id or token already exists")
            if next_index > 65535:
                raise ValueError("No safe monster class index remains")
            additions.append({**template, "Id": ident, "hcIdx": str(next_index),
                              "Code": token, "NextInClass": "",
                              "NameStr": f"d2lab_monster_{token.lower()}_name"})
            additions2.append({**template2, "Id": ident})
            ids.add(ident.casefold())
            ids2.add(ident.casefold())
            codes.add(token.casefold())
            next_index += 1
        return additions, additions2

    def _render_monsters(self, tables, monsters):
        monsters = self._validated_monster_definitions(monsters)
        if any(path not in tables for path in (_MONSTATS, _MONSTATS2)):
            raise ValueError("Authenticated monster tables are unavailable")
        header, rows = _tabular_rows(
            tables[_MONSTATS], "monstats.txt",
            ("Id", "hcIdx", "Code", "NameStr", "NextInClass"))
        header2, rows2 = _tabular_rows(tables[_MONSTATS2], "monstats2.txt", ("Id",))
        template = self._exact_source_row(rows, "Id", "brute2", "Brute monster")
        template2 = self._exact_source_row(rows2, "Id", "brute2", "Brute animation definition")
        ids = {row["Id"].casefold() for row in rows}
        ids2 = {row["Id"].casefold() for row in rows2}
        codes = {row["Code"].casefold() for row in rows}
        indexes = {int(row["hcIdx"]) for row in rows if row["hcIdx"].isdigit()}
        next_index = max(indexes, default=-1) + 1
        additions, additions2 = self._monster_row_additions(
            monsters, template, template2, ids, ids2, codes, next_index)
        return (self._append_tsv_rows(tables[_MONSTATS], header, additions),
                self._append_tsv_rows(tables[_MONSTATS2], header2, additions2))

    @staticmethod
    def _validate_unique_display_names(source, tables, unique_items):
        strings = {key.decode("cp1252").casefold(): value
                   for key, value in mod_strings._parse(source)[1]}
        _, rows = _tabular_rows(tables[_UNIQUE_ITEMS], "uniqueitems.txt", ("index",))
        authored_keys = {_unique_string_key(index).casefold()
                         for index in range(1, len(unique_items) + 1)}
        existing_names = set()
        for row in rows:
            key = row["index"].casefold()
            value = strings.get(key)
            if key not in authored_keys and value:
                existing_names.add(value.decode("utf-8", errors="replace").casefold())
        if any(item["name"].casefold() in existing_names for item in unique_items):
            raise ValueError("Unique item display name already exists")

    @staticmethod
    def _skill_localization_entries(tables, skills):
        _, rows = _tabular_rows(tables[_SKILLS], "skills.txt", ("skill", "Id"))
        by_name = {}
        for row in rows:
            by_name.setdefault(row["skill"].casefold(), []).append(row)
        entries = []
        for skill in skills:
            matches = by_name.get(skill["name"].casefold(), [])
            if len(matches) != 1:
                raise ValueError("Authored skill ID is missing or ambiguous")
            keys = _skill_string_keys(int(matches[0]["Id"]))
            entries.extend(((keys["name"], skill["name"]),
                            (keys["short"], skill["short_description"]),
                            (keys["long"], skill["description"]),
                            (keys["alt"], skill["description"])))
        return entries

    def _render_localization(self, baseline, tables, unique_items, skills, monsters, row_templates):
        source = self._source_localization_bytes(baseline)
        entries = [(_unique_string_key(index), item["name"])
                   for index, item in enumerate(unique_items, 1)]
        if unique_items:
            self._validate_unique_display_names(source, tables, unique_items)
        if skills:
            entries.extend(self._skill_localization_entries(tables, skills))
        entries.extend((f"d2lab_monster_{monster['token'].lower()}_name", monster["name"])
                       for monster in monsters)
        entries.extend(
            (entry["cells"]["namestr"], entry["cells"]["*name"])
            for operation in row_templates
            if operation["table"] == "data/global/excel/misc.txt"
            for entry in operation["additions"])
        entries.extend(
            (localized["key"], localized["value"])
            for operation in row_templates
            if operation["table"] == "data/global/excel/misc.txt"
            for entry in operation["additions"]
            for localized in entry.get("localizations", []))
        multiline_keys = {
            localized["key"]
            for operation in row_templates
            if operation["table"] == "data/global/excel/misc.txt"
            for entry in operation["additions"]
            for localized in entry.get("localizations", [])
            if "\n" in localized["value"]
        }
        return mod_strings.add_english(
            source, entries, allow_multiline_keys=multiline_keys)

    def _render_skill_and_monster_tables(self, result, skills, monsters, legacy):
        if skills:
            if _SKILLS not in result or _SKILLDESC not in result or _MISSILES not in result:
                raise ValueError("Authenticated skill or missile table is unavailable")
            renderer = self._render_legacy_skills if legacy else self._render_skills
            result[_SKILLS], result[_SKILLDESC], monster_data = renderer(result, skills)
            if monster_data is not None:
                result[_MONSTATS] = monster_data
        elif not isinstance(skills, (list, type(None))):
            raise ValueError("Invalid skill list")
        if monsters:
            if legacy:
                raise ValueError("Cannot add localized monster to a legacy workspace")
            result[_MONSTATS], result[_MONSTATS2] = self._render_monsters(result, monsters)
        elif not isinstance(monsters, (list, type(None))):
            raise ValueError("Invalid monster list")

    def _render_authored_tables(self, result, baseline, recipes, unique_items, skills,
                                monsters, legacy):
        if recipes:
            if _CUBE not in result:
                raise ValueError("Authenticated cubemain.txt is unavailable")
            result[_CUBE] = self._render_recipes(result[_CUBE], recipes, baseline)
        elif not isinstance(recipes, (list, type(None))):
            raise ValueError("Invalid recipe list")
        if unique_items:
            if _UNIQUE_ITEMS not in result:
                raise ValueError("Authenticated uniqueitems.txt is unavailable")
            result[_UNIQUE_ITEMS] = self._render_unique_items(
                result[_UNIQUE_ITEMS], unique_items, result, legacy=legacy)
        elif not isinstance(unique_items, (list, type(None))):
            raise ValueError("Invalid unique item list")
        self._render_skill_and_monster_tables(result, skills, monsters, legacy)

    def _render(self, baseline, changes, recipes=None, unique_items=None, skills=None,
                monsters=None, *, legacy=False, visual_assets=None, row_templates=None):
        result = self._baseline_bytes(baseline)
        for change in changes:
            table = change["table"]
            result[table] = _replace(result[table], change, change["expected"])
        for operation in _validated_row_templates(row_templates or [], baseline):
            path = operation["table"]
            result[path] = _template_rows(result[path], operation)
        self._render_authored_tables(
            result, baseline, recipes, unique_items, skills, monsters, legacy)
        if (unique_items or skills or monsters or any(
                op["table"] == "data/global/excel/misc.txt" for op in row_templates or [])) and not legacy:
            result[_PATCHSTRING] = self._render_localization(
                baseline, result, unique_items or [], skills or [], monsters or [],
                row_templates or [])
        for monster in monsters or []:
            for record in monster.get("asset_files", []):
                asset_path = record["path"]
                if asset_path in result:
                    raise ValueError("Monster asset collides with rendered table")
                blob = self._checked_file(
                    self.root / "asset-blobs" / record["sha256"], record)
                result[asset_path] = blob
        for record in visual_assets or []:
            asset_path = record["path"]
            if asset_path in result:
                raise ValueError("Visual asset collides with rendered file")
            result[asset_path] = self._checked_file(
                self.root / "asset-blobs" / record["sha256"], record)
        return result

    def _workspace_legacy_mode(self, state, baseline, recipes, unique_items, skills, monsters):
        visual_assets = state.get("visual_assets", [])
        row_templates = state.get("row_templates", [])
        legacy = state.get("legacy")
        if legacy is None:
            legacy_id = self._workspace_id(
                baseline, state["changes"], recipes, unique_items, skills, monsters,
                visual_assets=visual_assets, row_templates=row_templates, legacy=True)
            localized_id = self._workspace_id(
                baseline, state["changes"], recipes, unique_items, skills, monsters,
                visual_assets=visual_assets, row_templates=row_templates)
            if state["workspace_revision"] == legacy_id and legacy_id != localized_id:
                legacy = True
            elif state["workspace_revision"] == localized_id:
                legacy = baseline.get("localization_source") is None and bool(unique_items or skills)
            else:
                raise ValueError("Workspace state identity mismatch")
        if type(legacy) is not bool:
            raise ValueError("Invalid workspace localization mode")
        if legacy and monsters:
            raise ValueError("Legacy workspace cannot contain localized monsters")
        expected = self._workspace_id(
            baseline, state["changes"], recipes, unique_items, skills, monsters,
            visual_assets=visual_assets, row_templates=row_templates, legacy=legacy)
        if state["workspace_revision"] != expected:
            raise ValueError("Workspace state identity mismatch")
        return legacy

    def _validate_workspace_history(self, state, baseline):
        if state["head"] != baseline["baseline_id"]:
            self._revision(state["head"], baseline)
        published = state.get("published_revisions")
        if published is None:
            return
        if not isinstance(published, list):
            raise ValueError("Invalid workspace publication history")
        for revision in published:
            _identifier(revision, "mod_")
        if len(published) != len(set(published)):
            raise ValueError("Duplicate workspace publication")
        if state["head"] != baseline["baseline_id"] and state["head"] not in published:
            raise ValueError("Workspace head is missing from publication history")

    def _workspace(self, name, baseline):
        state = _load_json(_safe_store_path(self, self.root / "workspaces" / _name(name) / "state.json"))
        if state.get("name") != name or state.get("baseline_id") != baseline["baseline_id"]:
            raise ValueError("Workspace identity mismatch")
        self._validate_changes(state["changes"], baseline)
        _validated_row_templates(state.get("row_templates", []), baseline)
        recipes, unique_items, skills, monsters = (
            state.get(field, []) for field in ("recipes", "unique_items", "skills", "monsters"))
        visual_assets = state.get("visual_assets", [])
        for field, entries in (("recipe", recipes), ("unique item", unique_items),
                               ("skill", skills), ("monster", monsters)):
            if not isinstance(entries, list):
                raise ValueError(f"Invalid workspace {field} list")
        if visual_assets:
            visual_assets = _validated_visual_asset_records(visual_assets)
        elif not isinstance(visual_assets, list):
            raise ValueError("Invalid workspace visual asset list")
        self._validate_workspace_history(state, baseline)
        legacy = self._workspace_legacy_mode(
            state, baseline, recipes, unique_items, skills, monsters)
        state = {**state, "legacy": legacy}
        self._render(baseline, state["changes"], recipes, unique_items, skills, monsters,
                     legacy=self._legacy_rendering(state), visual_assets=visual_assets,
                     row_templates=state.get("row_templates", []))
        return state

    @staticmethod
    def _legacy_rendering(state):
        return state["legacy"] and bool(state.get("unique_items") or state.get("skills"))

    @staticmethod
    def _workspace_id(baseline, changes, recipes=None, unique_items=None, skills=None,
                      monsters=None, *, legacy=False, visual_assets=None, row_templates=None):
        identity = {"baseline_id": baseline["baseline_id"], "changes": changes}
        source = baseline.get("localization_source")
        if source is not None and not legacy:
            identity["localization_source_id"] = source["id"]
        for field, value in zip(
                ("recipes", "unique_items", "skills", "monsters", "visual_assets",
                 "row_templates"),
                (recipes, unique_items, skills, monsters, visual_assets, row_templates)):
            if value:
                identity[field] = value
        return "mod_" + _digest(identity)

    @staticmethod
    def _revision_operations(revision):
        source = revision or {}
        changes = source.get("changes", [])
        operations = tuple(source.get(field, [])
                          for field in ("recipes", "unique_items", "skills", "monsters"))
        return changes, operations

    @_locked
    def create(self, name, from_revision=None) -> dict:
        """Fork baseline or published revision into a separately mutable workspace."""
        baseline = self._manifest()
        _name(name)
        revision = self._revision(from_revision, baseline) if from_revision is not None else None
        changes, operations = self._revision_operations(revision)
        recipes, unique_items, skills, monsters = operations
        visual_assets = revision.get("visual_assets", []) if revision else []
        row_templates = revision.get("row_templates", []) if revision else []
        legacy = bool(revision and (unique_items or skills) and
                      "localization_source_id" not in revision)
        state = {"name": name, "baseline_id": baseline["baseline_id"],
                 "head": from_revision or baseline["baseline_id"],
                 "changes": changes,
                 "workspace_revision": self._workspace_id(
                     baseline, changes, recipes, unique_items, skills, monsters,
                     visual_assets=visual_assets, row_templates=row_templates, legacy=legacy),
                 "published_revisions": [from_revision] if from_revision is not None else [],
                 "legacy": legacy}
        for field, value in zip(
                ("recipes", "unique_items", "skills", "monsters"), operations):
            if value:
                state[field] = value
        if visual_assets:
            state["visual_assets"] = visual_assets
        if row_templates:
            state["row_templates"] = row_templates
        destination = self.root / "workspaces" / name
        if destination.exists():
            raise ValueError("Workspace already exists")
        staging = Path(tempfile.mkdtemp(prefix=".workspace-", dir=destination.parent))
        try:
            _atomic_json(staging / "state.json", state)
            staging.rename(destination)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        return self.inspect(name=name)

    def inspect(self, name=None, revision=None) -> dict:
        """Return one workspace, revision or baseline metadata (never table payloads)."""
        if name is not None and revision is not None:
            raise ValueError("Choose workspace or revision")
        baseline = self._manifest()
        if name is not None:
            state = self._workspace(name, baseline)
            return {**state, "change_count": len(state["changes"]),
                    "recipe_count": len(state.get("recipes", [])),
                    "unique_item_count": len(state.get("unique_items", [])),
                    "skill_count": len(state.get("skills", [])),
                    "monster_count": len(state.get("monsters", [])),
                    "asset_file_count": sum(len(monster.get("asset_files", []))
                                            for monster in state.get("monsters", [])),
                    "visual_asset_count": len(state.get("visual_assets", [])),
                    "table_count": len(baseline["files"])}
        if revision is not None:
            return self._revision(revision, baseline)
        return baseline

    def _published_revisions(self, name, state, baseline):
        published = state.get("published_revisions")
        if published is not None:
            return list(published)

        # Legacy workspaces have no history field. Recover only revisions whose
        # immutable manifest names this original publisher, plus the current head.
        revisions = set()
        root = _safe_store_path(self, self.root / "revisions")
        for directory in root.iterdir():
            if not directory.name.startswith("mod_") or not directory.is_dir():
                continue
            try:
                revision = _identifier(directory.name, "mod_")
            except ValueError:
                continue
            manifest = _load_json(_safe_store_path(self, directory / "manifest.json"))
            if (isinstance(manifest, dict) and manifest.get("schema_version") == 1 and
                    manifest.get("revision") == revision and
                    manifest.get("baseline_id") == baseline["baseline_id"] and
                    manifest.get("name") == name):
                revisions.add(revision)
        if state["head"] != baseline["baseline_id"]:
            revisions.add(state["head"])
        return sorted(revisions)

    def list(self) -> dict:
        """List names and compact workspace identities; no table data."""
        baseline = self._manifest()
        names = sorted(p.name for p in (self.root / "workspaces").iterdir())
        return {"baseline_id": baseline["baseline_id"], "workspaces": [
            {"name": name, "head": state["head"], "workspace_revision": state["workspace_revision"],
             "change_count": len(state["changes"]), "recipe_count": len(state.get("recipes", [])),
             "unique_item_count": len(state.get("unique_items", [])),
             "skill_count": len(state.get("skills", [])),
             "monster_count": len(state.get("monsters", [])),
             "asset_file_count": sum(len(monster.get("asset_files", []))
                                     for monster in state.get("monsters", [])),
             "visual_asset_count": len(state.get("visual_assets", [])),
             "published_revisions": self._published_revisions(name, state, baseline)}
            for name in names for state in [self._workspace(name, baseline)]]}

    def _browse_publications(self, name, state, baseline_id, head):
        published = state.get("published_revisions")
        if published is None:
            published = self._published_revisions(name, state, {"baseline_id": baseline_id})
        elif (not isinstance(published, list) or
              any(not isinstance(revision, str) for revision in published)):
            raise ValueError("Invalid workspace publication history")
        for revision in published:
            _identifier(revision, "mod_")
        if (len(published) != len(set(published)) or
                (head != baseline_id and head not in published)):
            raise ValueError("Workspace head is missing from publication history")
        return published

    def _browse_workspace_choice(self, directory, baseline_id):
        name = _name(directory.name)
        state = _load_json(_safe_store_path(self, directory / "state.json"))
        if (not isinstance(state, dict) or state.get("name") != name or
                state.get("baseline_id") != baseline_id):
            raise ValueError("Workspace identity mismatch")
        head = state.get("head")
        if head != baseline_id:
            _identifier(head, "mod_")
        published = self._browse_publications(name, state, baseline_id, head)
        return {"name": name, "head": head, "published_revisions": published}

    def browse_choices(self) -> dict:
        """Read selector metadata only; this is not an integrity or activation check."""
        manifest = _load_json(_safe_store_path(self, self.root / "manifest.json"))
        if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
            raise ValueError("Unknown baseline schema")
        baseline_id = _identifier(manifest.get("baseline_id"), "base_")
        root = _safe_store_path(self, self.root / "workspaces")
        workspaces = []
        for directory in sorted(root.iterdir(), key=lambda path: path.name):
            _safe_store_path(self, directory)
            if not directory.is_dir():
                raise ValueError("Invalid workspace entry")
            workspaces.append(self._browse_workspace_choice(directory, baseline_id))
        return {"baseline_id": baseline_id, "workspaces": workspaces}

    def table(self, table, key_column=None, key=None, name=None, revision=None) -> dict:
        """Inspect one table or uniquely keyed row; values are decoded cp1252."""
        if name is not None and revision is not None:
            raise ValueError("Choose workspace or revision")
        baseline = self._manifest()
        path = _table(table)
        if path not in {f["path"] for f in baseline["files"]}:
            raise ValueError("Unknown source table")
        if name is not None:
            state = self._workspace(name, baseline)
            data = self._render(baseline, state["changes"], state.get("recipes", []),
                                state.get("unique_items", []), state.get("skills", []),
                                state.get("monsters", []),
                                visual_assets=state.get("visual_assets", []),
                                row_templates=state.get("row_templates", []),
                                legacy=self._legacy_rendering(state))[path]
        elif revision is not None:
            manifest = self._revision(revision, baseline)
            record = next(f for f in manifest["files"] if f["path"] == path)
            data = self._checked_file(self.root / "revisions" / revision / path, record)
        else:
            record = next(f for f in baseline["files"] if f["path"] == path)
            data = self._checked_file(self.root / "baselines" / baseline["baseline_id"] / path, record)
        lines = _records(data)
        columns = lines[0].rstrip(b"\r\n").decode("cp1252").split("\t")
        if key is None:
            if key_column is not None:
                raise ValueError("key_column requires key")
            return {"table": path, "columns": columns, "row_count": len(lines) - 1, "sha256": _hash(data), "bytes": len(data)}
        if key_column is None:
            raise ValueError("key requires key_column")
        _, index, _, _ = _row(data, key_column, key, key_column)
        values = lines[index].rstrip(b"\r\n").decode("cp1252").split("\t")
        return {"table": path, "key_column": key_column, "key": key, "row": dict(zip(columns, values))}

    @_locked
    def patch(self, name, edits: list[dict], expected_revision: str, *, dry_run=False) -> dict:
        """Preflight whole batch against CURRENT workspace, then atomically CAS update.

        Each edit has table, key_column, key, column, expected (current), value.
        Existing deltas retain their original baseline expected, not prior values.
        """
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        if not isinstance(edits, list) or not 1 <= len(edits) <= 64:
            raise ValueError("Patch batch must contain 1–64 edits")
        if any(not isinstance(c, dict) or set(c) != {"table", "key_column", "key", "column", "expected", "value"} for c in edits):
            raise ValueError("Invalid change")
        self._validate_changes(sorted(edits, key=lambda c: (c["table"], c["key_column"], c["key"], c["column"])), baseline)
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        current = self._render(
            baseline, state["changes"], recipes, unique_items, skills, monsters,
            legacy=self._legacy_rendering(state), visual_assets=state.get("visual_assets", []),
            row_templates=state.get("row_templates", []))
        originals = self._baseline_bytes(baseline)
        changes = {tuple(c[f] for f in ("table", "key_column", "key", "column")): dict(c) for c in state["changes"]}
        for edit in edits:
            target = tuple(edit[f] for f in ("table", "key_column", "key", "column"))
            table = edit["table"]
            current[table] = _replace(current[table], edit, edit["expected"])
            if target in changes:
                original = changes[target]["expected"]
            else:
                _, _, _, original_bytes = _row(originals[table], edit["key_column"], edit["key"], edit["column"])
                original = original_bytes.decode("cp1252")
            if edit["value"] == original:
                changes.pop(target, None)
            else:
                changes[target] = {**edit, "expected": original}
        flattened = sorted(changes.values(), key=lambda c: (c["table"], c["key_column"], c["key"], c["column"]))
        # Also reject two distinct key selectors that resolve to the same physical cell.
        if self._render(
                baseline, flattened, recipes, unique_items, skills, monsters,
                legacy=self._legacy_rendering(state),
                visual_assets=state.get("visual_assets", []),
                row_templates=state.get("row_templates", [])) != current:
            raise ValueError("Conflicting edits resolve to the same table cell")
        state = {**state, "changes": flattened,
                 "workspace_revision": self._workspace_id(
                     baseline, flattened, recipes, unique_items, skills, monsters,
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []), legacy=state["legacy"])}
        if dry_run:
            return {**state, "change_count": len(flattened), "recipe_count": len(recipes),
                    "unique_item_count": len(unique_items), "skill_count": len(skills),
                    "monster_count": len(monsters),
                    "table_count": len(baseline["files"])}
        _atomic_json(self.root / "workspaces" / name / "state.json", state)
        return self.inspect(name=name)

    @_locked
    def clone_rows(self, name, operation: dict, expected_revision: str) -> dict:
        """CAS-copy or replace a source-table row-template operation.

        Each target supplies only non-key column overrides; authenticated
        template and original table determine every other cell. A new operation
        replaces any existing operation for the same source table.
        """
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        _validated_row_templates([operation], baseline)
        operations = [template for template in state.get("row_templates", [])
                      if template["table"] != operation["table"]]
        operations.append(operation)
        operations.sort(key=lambda item: item["table"])
        _validated_row_templates(operations, baseline)
        self._render(
            baseline, state["changes"], state.get("recipes", []),
            state.get("unique_items", []), state.get("skills", []),
            state.get("monsters", []), legacy=self._legacy_rendering(state),
            visual_assets=state.get("visual_assets", []), row_templates=operations)
        updated = {**state, "row_templates": operations,
                   "workspace_revision": self._workspace_id(
                       baseline, state["changes"], state.get("recipes", []),
                       state.get("unique_items", []), state.get("skills", []),
                       state.get("monsters", []), legacy=state["legacy"],
                       visual_assets=state.get("visual_assets", []),
                       row_templates=operations)}
        _atomic_json(self.root / "workspaces" / name / "state.json", updated)
        return self.inspect(name=name)

    @_locked
    def add_cube_recipe(self, name, recipe: dict, expected_revision: str, *, dry_run=False) -> dict:
        """Append one exact-code cube recipe through a typed, revisioned operation."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        normalized = _recipe_spec(recipe)
        updated = [*recipes, normalized]
        if len(updated) > 64:
            raise ValueError("Workspace supports at most 64 cube recipes")
        self._render(baseline, state["changes"], updated, unique_items, skills, monsters,
                     legacy=self._legacy_rendering(state),
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []))
        state = {**state, "recipes": updated,
                 "workspace_revision": self._workspace_id(
                     baseline, state["changes"], updated, unique_items, skills, monsters,
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []), legacy=state["legacy"])}
        if dry_run:
            return {**state, "change_count": len(state["changes"]), "recipe_count": len(updated),
                    "unique_item_count": len(unique_items), "skill_count": len(skills),
                    "monster_count": len(monsters),
                    "table_count": len(baseline["files"])}
        _atomic_json(self.root / "workspaces" / name / "state.json", state)
        return self.inspect(name=name)

    @_locked
    def add_unique_item(self, name, unique_item: dict, expected_revision: str, *, dry_run=False) -> dict:
        """Append one typed, direct-stat unique item using an existing base item."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        if state["legacy"] and (unique_items or skills):
            raise ValueError("Cannot add localized content to a legacy unique/skill workspace")
        normalized = _unique_item_spec(unique_item)
        updated = [*unique_items, normalized]
        if len(updated) > 16:
            raise ValueError("Workspace supports at most 16 new unique items")
        self._render(baseline, state["changes"], recipes, updated, skills, monsters,
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []))
        state = {**state, "unique_items": updated, "legacy": False,
                 "workspace_revision": self._workspace_id(
                     baseline, state["changes"], recipes, updated, skills, monsters,
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []))}
        if dry_run:
            return {**state, "change_count": len(state["changes"]), "recipe_count": len(recipes),
                    "unique_item_count": len(updated), "skill_count": len(skills),
                    "monster_count": len(monsters),
                    "table_count": len(baseline["files"])}
        _atomic_json(self.root / "workspaces" / name / "state.json", state)
        return self.inspect(name=name)

    @_locked
    def attach_monster_assets(self, name, token, package_dir, expected_revision) -> dict:
        """Attach one validated, immutable D2 monster animation package to a workspace."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        monster_index = next((index for index, monster in enumerate(state.get("monsters", []))
                              if monster["token"] == token), None)
        if monster_index is None:
            raise ValueError("Monster token is not registered in this workspace")
        if state["monsters"][monster_index].get("asset_files"):
            raise ValueError("Monster already has attached assets")
        records, payloads = _read_monster_asset_package(package_dir, token)
        existing = ({record["path"] for monster in state.get("monsters", [])
                     for record in monster.get("asset_files", [])} |
                    {record["path"] for record in state.get("visual_assets", [])})
        if existing.intersection(record["path"] for record in records):
            raise ValueError("Monster asset path is already attached")
        for record in records:
            blob = _safe_store_path(self, self.root / "asset-blobs" / record["sha256"])
            _atomic_bytes(blob, payloads[record["path"]])
        monsters = list(state["monsters"])
        monsters[monster_index] = {**monsters[monster_index], "asset_files": records}
        new_state = {**state, "monsters": monsters,
                     "workspace_revision": self._workspace_id(
                         baseline, state["changes"], state.get("recipes", []),
                         state.get("unique_items", []), state.get("skills", []), monsters,
                         visual_assets=state.get("visual_assets", []),
                         row_templates=state.get("row_templates", []))}
        self._render(baseline, state["changes"], state.get("recipes", []),
                     state.get("unique_items", []), state.get("skills", []), monsters,
                     legacy=self._legacy_rendering(state),
                     visual_assets=state.get("visual_assets", []),
                     row_templates=state.get("row_templates", []))
        _atomic_json(self.root / "workspaces" / name / "state.json", new_state)
        return self.inspect(name=name)

    @_locked
    def attach_visual_assets(self, name, package_dir, expected_revision) -> dict:
        """Attach allowlisted hash-checked item, skill, or arena tile assets."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        records, payloads = _read_visual_asset_package(package_dir)
        existing = ({record["path"] for record in state.get("visual_assets", [])} |
                    {record["path"] for monster in state.get("monsters", [])
                     for record in monster.get("asset_files", [])})
        if existing.intersection(record["path"] for record in records):
            raise ValueError("Visual asset path is already attached")
        for record in records:
            blob = _safe_store_path(self, self.root / "asset-blobs" / record["sha256"])
            _atomic_bytes(blob, payloads[record["path"]])
        visual_assets = sorted([*state.get("visual_assets", []), *records],
                               key=lambda record: record["path"])
        new_state = {**state, "visual_assets": visual_assets,
                     "workspace_revision": self._workspace_id(
                         baseline, state["changes"], state.get("recipes", []),
                         state.get("unique_items", []), state.get("skills", []),
                         state.get("monsters", []), visual_assets=visual_assets,
                         row_templates=state.get("row_templates", []),
                         legacy=state["legacy"])}
        self._render(baseline, state["changes"], state.get("recipes", []),
                     state.get("unique_items", []), state.get("skills", []),
                     state.get("monsters", []), legacy=self._legacy_rendering(state),
                     visual_assets=visual_assets, row_templates=state.get("row_templates", []))
        _atomic_json(self.root / "workspaces" / name / "state.json", new_state)
        return self.inspect(name=name)

    @_locked
    def add_monster(self, name, monster: dict, expected_revision: str, *, dry_run=False) -> dict:
        """Clone the authenticated brute2 row pair with one unique animation token."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        if state["legacy"] and (state.get("unique_items") or state.get("skills")):
            raise ValueError("Cannot add localized content to a legacy unique/skill workspace")
        definition = _monster_spec(monster)
        monsters = [*state.get("monsters", []), definition]
        rendered = self._render(
            baseline, state["changes"], state.get("recipes", []),
            state.get("unique_items", []), state.get("skills", []), monsters,
            visual_assets=state.get("visual_assets", []),
            row_templates=state.get("row_templates", []))
        _, rows = _tabular_rows(rendered[_MONSTATS], "monstats.txt", ("Id", "hcIdx", "Code"))
        authored = self._exact_source_row(
            rows, "Id", f"d2labbrute{definition['token'].lower()}", "authored monster")
        new_state = {**state, "monsters": monsters, "legacy": False,
                     "workspace_revision": self._workspace_id(
                         baseline, state["changes"], state.get("recipes", []),
                         state.get("unique_items", []), state.get("skills", []), monsters,
                         visual_assets=state.get("visual_assets", []),
                         row_templates=state.get("row_templates", []))}
        receipt = {"class_id": authored["Id"], "token": authored["Code"],
                   "hcIdx": int(authored["hcIdx"])}
        if dry_run:
            return {**new_state, "change_count": len(state["changes"]),
                    "recipe_count": len(state.get("recipes", [])),
                    "unique_item_count": len(state.get("unique_items", [])),
                    "skill_count": len(state.get("skills", [])),
                    "monster_count": len(monsters), "table_count": len(baseline["files"]),
                    **receipt}
        _atomic_json(self.root / "workspaces" / name / "state.json", new_state)
        return {**self.inspect(name=name), **receipt}

    @staticmethod
    def _authored_skill_receipts(rendered, skills):
        _, skill_rows = _tabular_rows(rendered[_SKILLS], "skills.txt", ("skill", "Id"))
        authored_rows = {row["skill"]: row for row in skill_rows}
        _, descriptions = _tabular_rows(
            rendered[_SKILLDESC], "skilldesc.txt",
            ("skilldesc", "SkillPage", "SkillRow", "SkillColumn"))
        authored_descriptions = {row["skilldesc"].casefold(): row for row in descriptions}
        receipts = []
        for skill in skills:
            authored = authored_rows.get(skill["name"])
            if authored is None:
                raise ValueError("Authored skill was not rendered")
            descriptor = authored_descriptions.get(skill["name"].casefold())
            if descriptor is None:
                raise ValueError("Authored skill description was not rendered")
            receipts.append({
                "skill_id": int(authored["Id"]),
                "skill_tree_slot": {
                    "page": int(descriptor["SkillPage"]),
                    "row": int(descriptor["SkillRow"]),
                    "column": int(descriptor["SkillColumn"]),
                },
            })
        return receipts

    def _add_skill_batch(self, name, definitions, expected_revision, *,
                         dry_run=False, require_empty=False):
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        if state["legacy"] and (unique_items or skills):
            raise ValueError("Cannot add localized content to a legacy unique/skill workspace")
        if require_empty and skills:
            raise ValueError("Hydra palette requires a workspace without authored skills")
        updated = [*skills, *definitions]
        rendered = self._render(
            baseline, state["changes"], recipes, unique_items, updated, monsters,
            visual_assets=state.get("visual_assets", []),
            row_templates=state.get("row_templates", []))
        receipts = self._authored_skill_receipts(rendered, definitions)
        new_state = {**state, "skills": updated, "legacy": False,
                     "workspace_revision": self._workspace_id(
                         baseline, state["changes"], recipes, unique_items, updated, monsters,
                         visual_assets=state.get("visual_assets", []),
                         row_templates=state.get("row_templates", []))}
        result = {**new_state, "change_count": len(state["changes"]), "recipe_count": len(recipes),
                  "unique_item_count": len(unique_items), "skill_count": len(updated),
                  "monster_count": len(monsters),
                  "table_count": len(baseline["files"])}
        if dry_run:
            return result, receipts
        _atomic_json(self.root / "workspaces" / name / "state.json", new_state)
        return self.inspect(name=name), receipts

    @_locked
    def add_skill(self, name, skill_definition: dict, expected_revision: str, *, dry_run=False) -> dict:
        """Append one Sorceress skill via the typed Fire Bolt or Cold Hydra template."""
        definition = _skill_spec(skill_definition)
        state, receipts = self._add_skill_batch(
            name, [definition], expected_revision, dry_run=dry_run)
        return {**state, **receipts[0]}

    @_locked
    def add_hydra_palette(self, name, expected_revision: str, *, dry_run=False) -> dict:
        """Append one atomic set of eight localized Cold Hydra palette test skills."""
        definitions = [
            _skill_spec({
                "name": f"Hydra Palette {level}",
                "short_description": f"Hydra palette test {level}.",
                "description": f"Summons a Hydra using monster transform code {level}.",
                "template": "cold_hydra",
                "monster_trans_level": level,
            })
            for level in range(len(_HYDRA_PALETTE_TREE_SLOTS))
        ]
        state, receipts = self._add_skill_batch(
            name, definitions, expected_revision, dry_run=dry_run, require_empty=True)
        palette = [
            {"name": definition["name"],
             "monster_trans_level": definition["monster_trans_level"], **receipt}
            for definition, receipt in zip(definitions, receipts)
        ]
        return {**state, "palette": palette}

    def _revision_manifest(self, baseline, name, state, files):
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        visual_assets = state.get("visual_assets", [])
        manifest = {"schema_version": 1, "baseline_id": baseline["baseline_id"],
                    "name": name, "source_archives": baseline["source_archives"],
                    "files": files, "changes": state["changes"]}
        source_id = None
        source = baseline.get("localization_source")
        if (unique_items or skills or monsters or any(
                op["table"] == "data/global/excel/misc.txt"
                for op in state.get("row_templates", []))) and not self._legacy_rendering(state):
            if source is None:
                raise ValueError("Authenticated English localized string-table source is required")
            source_id = source["id"]
            manifest["localization_source_id"] = source_id
        for field, value in zip(
                ("recipes", "unique_items", "skills", "monsters", "visual_assets",
                 "row_templates"),
                (recipes, unique_items, skills, monsters, visual_assets,
                 state.get("row_templates", []))):
            if value:
                manifest[field] = value
        manifest["revision"] = "mod_" + _digest(
            self._revision_identity(manifest, source_id))
        return manifest

    def _materialize_revision(self, destination, baseline, files, data, manifest):
        revision = manifest["revision"]
        if destination.exists():
            self._revision(revision, baseline)
            return
        staging = Path(tempfile.mkdtemp(prefix=".revision-", dir=destination.parent))
        try:
            for file in files:
                target = staging / file["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data[file["path"]])
            _atomic_json(staging / "manifest.json", manifest)
            staging.rename(destination)
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    @_locked
    def publish(self, name, expected_revision: str) -> dict:
        """Materialize every baseline table into one immutable content-addressed revision."""
        baseline = self._manifest()
        state = self._workspace(name, baseline)
        if state["workspace_revision"] != expected_revision:
            raise ValueError("Stale workspace revision")
        recipes = state.get("recipes", [])
        unique_items = state.get("unique_items", [])
        skills = state.get("skills", [])
        monsters = state.get("monsters", [])
        data = self._render(
            baseline, state["changes"], recipes, unique_items, skills, monsters,
            legacy=self._legacy_rendering(state), visual_assets=state.get("visual_assets", []),
            row_templates=state.get("row_templates", []))
        files = [{"path": path, "sha256": _hash(data[path]), "bytes": len(data[path])}
                 for path in sorted(data)]
        manifest = self._revision_manifest(baseline, name, state, files)
        revision = manifest["revision"]
        destination = self.root / "revisions" / revision
        self._materialize_revision(destination, baseline, files, data, manifest)
        published = self._published_revisions(name, state, baseline)
        if revision not in published:
            published.append(revision)
        _atomic_json(self.root / "workspaces" / name / "state.json",
                     {**state, "head": revision, "published_revisions": published})
        return self._revision(revision, baseline)

    def revision_directory(self, revision: str) -> Path:
        """Validate the entire immutable candidate, then return its staging directory."""
        self._revision(revision, self._manifest())
        return self.root / "revisions" / revision
