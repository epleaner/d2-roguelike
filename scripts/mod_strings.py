"""Lossless writer for typed English entries in the S13 patchstring table."""
from __future__ import annotations

import binascii
import re
import struct


_HEADER = struct.Struct("<HHIBIII")
_NODE = struct.Struct("<BHIIIH")
_HEADER_SIZE = _HEADER.size
_NODE_SIZE = _NODE.size
_KEY = re.compile(rb"[a-z][a-z0-9_]{1,63}\Z")


def _hash_value(key: bytes, table_size: int) -> int:
    value = 0
    for byte in key:
        value = ((value << 4) + (byte if byte < 128 else byte - 256)) & 0xFFFFFFFF
        high = value & 0xF0000000
        if high:
            value = (value & 0x0FFFFFFF) ^ (high >> 24)
    return value % table_size


def _parse(data: bytes) -> tuple[int, list[tuple[bytes, bytes]]]:
    if not isinstance(data, bytes) or len(data) < _HEADER_SIZE:
        raise ValueError("Invalid or truncated TBL header")
    crc, row_count, hash_size, version, data_start, max_tries, file_size = _HEADER.unpack_from(data)
    if (not row_count or not hash_size or hash_size < row_count or version not in (0, 1)
            or file_size != len(data) or max_tries < 1 or max_tries > hash_size):
        raise ValueError("Unsupported TBL header values")
    expected_start = _HEADER_SIZE + row_count * 2 + hash_size * _NODE_SIZE
    if data_start != expected_start or data_start > file_size:
        raise ValueError("Unsupported TBL index and node layout")
    pool = data[data_start:]
    if binascii.crc_hqx(pool, 0xFFFF) != crc:
        raise ValueError("TBL string-pool checksum mismatch")

    indices = struct.unpack_from(f"<{row_count}H", data, _HEADER_SIZE)
    if any(slot >= hash_size for slot in indices) or len(set(indices)) != row_count:
        raise ValueError("Invalid TBL index map")
    rows: list[tuple[bytes, bytes] | None] = [None] * row_count
    key_offsets: list[int | None] = [None] * row_count
    slots: list[int | None] = [None] * row_count
    node_start = _HEADER_SIZE + row_count * 2
    for slot in range(hash_size):
        active, index, stored_hash, key_offset, value_offset, value_length = _NODE.unpack_from(
            data, node_start + slot * _NODE_SIZE)
        if active not in (0, 1):
            raise ValueError("Invalid TBL node active flag")
        if not active:
            continue
        if index >= row_count or rows[index] is not None:
            raise ValueError("Invalid or duplicate TBL row index")
        if indices[index] != slot or key_offset < data_start or value_offset < data_start:
            raise ValueError("TBL node does not match its index map")
        key_end = data.find(b"\0", key_offset, file_size)
        if key_end <= key_offset or value_offset != key_end + 1 or value_length < 1:
            raise ValueError("Invalid TBL key or value offset")
        value_end = value_offset + value_length
        if value_end > file_size or data[value_end - 1] != 0:
            raise ValueError("Truncated TBL value")
        key, value = data[key_offset:key_end], data[value_offset:value_end - 1]
        if b"\0" in value or stored_hash != _hash_value(key, hash_size):
            raise ValueError("Invalid TBL value or key hash")
        rows[index] = (key, value)
        key_offsets[index] = key_offset
        slots[index] = slot
    if any(row is None for row in rows):
        raise ValueError("TBL active node count differs from its index count")

    expected_offset = data_start
    occupied = set()
    max_collisions = 0
    for index, row in enumerate(rows):
        key, value = row
        if key_offsets[index] != expected_offset:
            raise ValueError("Unsupported non-contiguous TBL string pool")
        value_offset = expected_offset + len(key) + 1
        node_hash = _hash_value(key, hash_size)
        slot = node_hash
        collisions = 0
        while slot in occupied:
            slot = (slot + 1) % hash_size
            collisions += 1
        if slots[index] != slot:
            raise ValueError("Unsupported TBL hash probe layout")
        occupied.add(slot)
        max_collisions = max(max_collisions, collisions)
        expected_offset = value_offset + len(value) + 1
    if expected_offset != file_size or max_tries != max_collisions + 1:
        raise ValueError("Unsupported TBL pool length or collision metadata")
    return version, [row for row in rows if row is not None]


def _serialize(version: int, rows: list[tuple[bytes, bytes]]) -> bytes:
    count = len(rows)
    if not 1 <= count <= 0xFFFF:
        raise ValueError("TBL entry count exceeds the supported 16-bit index range")
    data_start = _HEADER_SIZE + count * 2 + count * _NODE_SIZE
    indices = [0] * count
    nodes = bytearray(count * _NODE_SIZE)
    pool = bytearray()
    occupied = set()
    max_collisions = 0
    for index, (key, value) in enumerate(rows):
        if not key or b"\0" in key or b"\0" in value or len(value) + 1 > 0xFFFF:
            raise ValueError("Invalid TBL entry bytes")
        key_offset = data_start + len(pool)
        value_offset = key_offset + len(key) + 1
        value_length = len(value) + 1
        hash_value = _hash_value(key, count)
        slot = hash_value
        collisions = 0
        while slot in occupied:
            slot = (slot + 1) % count
            collisions += 1
        occupied.add(slot)
        max_collisions = max(max_collisions, collisions)
        indices[index] = slot
        _NODE.pack_into(nodes, slot * _NODE_SIZE, 1, index, hash_value,
                        key_offset, value_offset, value_length)
        pool.extend(key)
        pool.append(0)
        pool.extend(value)
        pool.append(0)
    index_data = struct.pack(f"<{count}H", *indices)
    body = index_data + nodes + pool
    file_size = _HEADER_SIZE + len(body)
    header = _HEADER.pack(binascii.crc_hqx(pool, 0xFFFF), count, count, version,
                          data_start, max_collisions + 1, file_size)
    return header + body


def add_english(data: bytes, entries: list[tuple[str, str]], *,
                allow_multiline_keys=()) -> bytes:
    """Append collision-checked namespaced English strings to an S13 TBL."""
    version, rows = _parse(data)
    if not isinstance(entries, list):
        raise ValueError("Localized entries must be a list")
    if not entries:
        return data
    keys = {key.decode("cp1252").casefold() for key, _ in rows}
    additions = []
    for entry in entries:
        if not isinstance(entry, tuple) or len(entry) != 2:
            raise ValueError("Localized entry must contain a key and value")
        key, value = entry
        if not isinstance(key, str):
            raise ValueError("Localized key must be text")
        key_bytes = key.encode("ascii")
        if not _KEY.fullmatch(key_bytes) or key.casefold() in keys:
            raise ValueError("Invalid or colliding localized key")
        multiline = key in allow_multiline_keys
        if (not isinstance(value, str) or not value or
                any(ord(char) < 0x20 and not (multiline and char == "\n")
                    for char in value) or
                ("\n" in value and
                 (not multiline or value.count("\n") != 1 or
                  any(not line.strip() for line in value.split("\n"))))):
            raise ValueError("Localized value must be nonempty printable text")
        value_bytes = value.encode("utf-8")
        if len(value_bytes) + 1 > 0xFFFF:
            raise ValueError("Localized value exceeds the TBL length limit")
        keys.add(key.casefold())
        additions.append((key_bytes, value_bytes))
    return _serialize(version, [*rows, *additions])
