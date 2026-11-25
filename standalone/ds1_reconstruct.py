"""Rebuild pinned S14 DS1 edits from authenticated, player-owned source bytes.

The bounded v17/v18 layout follows DS1 Studio's MIT-licensed parser at
https://github.com/RoofooEvazan/ds1-studio/tree/d935da9082028ed2aa7d7fbf50185f723343b535

MIT License
Copyright (c) 2026 RoofooEvazan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
"""
from __future__ import annotations

import struct

_I32 = struct.Struct("<i")
_EMPTY = b"\x00" * 4
_TOWN_OBJECTS = frozenset((52, 102, 110, 417))
_NORTH_MARKER = (2, 492, 130, 41, 0)


def _layout(source: bytes):
    """Find semantic sections, rejecting unsupported or ambiguous DS1 layouts."""
    pos = 0

    def number():
        nonlocal pos
        if pos + 4 > len(source):
            raise ValueError("Truncated authenticated DS1")
        value = _I32.unpack_from(source, pos)[0]
        pos += 4
        return value

    version, width, height = number(), number() + 1, number() + 1
    if version not in (17, 18) or not 1 <= width <= 256 or not 1 <= height <= 256:
        raise ValueError("Unsupported authenticated DS1 header")
    cells = width * height
    number()  # Act (preserved)
    has_groups = number() in (1, 2)
    file_count = number()
    if not 0 <= file_count <= 128:
        raise ValueError("Invalid authenticated DS1 file list")
    for _ in range(file_count):
        end = source.find(b"\x00", pos)
        if end < pos or end - pos > 1024:
            raise ValueError("Invalid authenticated DS1 file name")
        pos = end + 1
    walls, floors = number(), number()
    if not 0 <= walls <= 4 or not 0 <= floors <= 2:
        raise ValueError("Invalid authenticated DS1 layers")
    wall_offsets = []
    for _ in range(walls):
        wall_offsets.append((pos, pos + cells * 4))
        pos += cells * 8
    pos += (floors + 1 + has_groups) * cells * 4
    if pos + 4 > len(source):
        raise ValueError("Truncated authenticated DS1 cells")
    count_offset = pos
    count = number()
    if not 0 <= count <= 4096 or pos + count * 20 > len(source):
        raise ValueError("Invalid authenticated DS1 object count")
    objects = []
    for _ in range(count):
        objects.append((_I32.unpack_from(source, pos)[0],
                        _I32.unpack_from(source, pos + 4)[0],
                        _I32.unpack_from(source, pos + 8)[0],
                        _I32.unpack_from(source, pos + 12)[0], pos))
        pos += 20
    end_objects = pos
    if has_groups:
        if version == 18:
            number()  # Groups header
        groups = number()
        if not 0 <= groups <= 4096 or pos + groups * 20 > len(source):
            raise ValueError("Invalid authenticated DS1 groups")
        pos += groups * 20
    path_count_offset = pos
    paths = number()
    if not 0 <= paths <= 4096:
        raise ValueError("Invalid authenticated DS1 path count")
    path_owners = []
    for _ in range(paths):
        points, x, y = number(), number(), number()
        if not 0 <= points <= 4096 or pos + points * 12 > len(source):
            raise ValueError("Invalid authenticated DS1 NPC path")
        path_owners.append((x, y))
        pos += points * 12
    if pos != len(source):
        raise ValueError("Unexpected authenticated DS1 trailing bytes")
    return width, height, wall_offsets, count_offset, objects, end_objects, path_count_offset, path_owners


def reconstruct(source: bytes, operation: str, wall_cells: list[list[int]]) -> bytes:
    """Apply only object filtering and complete wall-cell removal, never binary deltas."""
    if operation == "copy":
        if wall_cells:
            raise ValueError("Copied DS1 cannot have edits")
        return source
    (width, height, wall_offsets, count_offset, objects, end_objects,
     path_count_offset, path_owners) = _layout(source)
    if operation == "clear" or operation == "clear_walls":
        kept = [] if operation == "clear" else objects
    elif operation in ("town", "town_north"):
        north = operation == "town_north"
        kept = [row for row in objects if row[0] != 1 and
                not (row[0] == 2 and (row[1] in _TOWN_OBJECTS or north and row[1] == 2))]
    else:
        raise ValueError("Unsupported DS1 semantic edit")
    if path_owners:
        kept_offsets = {row[4] for row in kept}
        kept_positions = {(row[2], row[3]) for row in kept}
        removed_positions = {(row[2], row[3]) for row in objects if row[4] not in kept_offsets}
        if any(owner not in removed_positions or owner in kept_positions for owner in path_owners):
            raise ValueError("Authenticated DS1 has a surviving or orphan NPC path")
    output = bytearray(source[:count_offset])
    output += _I32.pack(len(kept) + (operation == "town_north"))
    for row in kept:
        output += source[row[4]:row[4] + 20]
    if operation == "town_north":
        output += struct.pack("<5i", *_NORTH_MARKER)
    output += source[end_objects:path_count_offset]
    output += _I32.pack(0)
    if len(wall_cells) != len({tuple(cell) for cell in wall_cells}):
        raise ValueError("Duplicate DS1 wall cell edit")
    for layer, x, y in wall_cells:
        if not 0 <= layer < len(wall_offsets) or not 0 <= x < width or not 0 <= y < height:
            raise ValueError("DS1 wall edit is outside the authenticated layout")
        tile, orientation = wall_offsets[layer]
        index = (y * width + x) * 4
        if (source[tile + index:tile + index + 4] == _EMPTY or
                source[orientation + index] not in (10, 11)):
            raise ValueError("Authenticated DS1 wall marker differs")
        output[tile + index:tile + index + 4] = _EMPTY
        output[orientation + index:orientation + index + 4] = _EMPTY
    return bytes(output)
