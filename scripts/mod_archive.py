"""Read season-owned Excel tables through StormLib; never modify an archive."""
from __future__ import annotations
from contextlib import contextmanager

import ctypes
import ctypes.util
import hashlib
from pathlib import Path, PurePosixPath
import os
import sys


def _storm_library():
    if os.name != 'nt':
        library = ctypes.util.find_library('storm')
        if not library:
            raise RuntimeError('StormLib is required for MPQ extraction (brew install stormlib)')
        return ctypes.CDLL(library)
    if getattr(sys, 'frozen', False):
        candidate = Path(__file__).resolve().parent / 'StormLib.dll'
        if not candidate.is_file():
            raise RuntimeError('The bundled StormLib.dll is missing')
    else:
        override = os.environ.get('STORMLIB_PATH')
        if override:
            candidate = Path(override)
            if not candidate.is_absolute() or not candidate.is_file():
                raise RuntimeError('STORMLIB_PATH must name an existing absolute StormLib DLL')
        else:
            candidates = (Path(__file__).resolve().parent / 'StormLib.dll',
                          Path(sys.executable).parent / 'StormLib.dll')
            candidate = next((path for path in candidates if path.is_file()), None)
            if candidate is None:
                raise RuntimeError('Install StormLib.dll beside the module or set STORMLIB_PATH')
    return ctypes.WinDLL(str(candidate))


def _windows_archive_path(storm, path):
    """Prefer the wide entry point; ANSI-only DLLs need a real ASCII short path."""
    wide = getattr(storm, 'SFileOpenArchiveW', None)
    if wide is not None:
        return wide, str(path), ctypes.c_wchar_p
    if str(path).isascii():
        return storm.SFileOpenArchive, os.fsencode(path), ctypes.c_char_p
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    get_short = kernel.GetShortPathNameW
    get_short.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
    get_short.restype = ctypes.c_uint32
    buffer = ctypes.create_unicode_buffer(32768)
    length = get_short(str(path), buffer, len(buffer))
    if not length or length >= len(buffer) or not buffer.value.isascii():
        raise RuntimeError('Unicode archive path requires SFileOpenArchiveW or an ASCII Windows short path')
    return storm.SFileOpenArchive, buffer.value.encode('ascii'), ctypes.c_char_p


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


@contextmanager
def _authenticated_archive(path, expected_sha256):
    path = Path(path).resolve(strict=True)
    if sha256(path) != expected_sha256:
        raise ValueError('Archive differs from authenticated source identity')
    storm = _storm_library()
    handle, dword = ctypes.c_void_p, ctypes.c_uint32
    signatures = {
        'SFileOpenArchive': ([ctypes.c_char_p, dword, dword, ctypes.POINTER(handle)], ctypes.c_bool),
        'SFileOpenFileEx': ([handle, ctypes.c_char_p, dword, ctypes.POINTER(handle)], ctypes.c_bool),
        'SFileGetFileSize': ([handle, ctypes.POINTER(dword)], dword),
        'SFileReadFile': ([handle, handle, dword, ctypes.POINTER(dword), handle], ctypes.c_bool),
        'SFileCloseFile': ([handle], ctypes.c_bool),
        'SFileCloseArchive': ([handle], ctypes.c_bool),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(storm, name)
        function.argtypes, function.restype = arguments, result
    opener, archive_path, path_type = (
        _windows_archive_path(storm, path) if os.name == 'nt'
        else (storm.SFileOpenArchive, os.fsencode(path), ctypes.c_char_p))
    opener.argtypes = [path_type, dword, dword, ctypes.POINTER(handle)]
    opener.restype = ctypes.c_bool
    archive = handle()
    if not opener(archive_path, 0, 0x100, ctypes.byref(archive)):
        raise ValueError('StormLib refused the authenticated archive')

    def read(name):
        file = handle()
        if not storm.SFileOpenFileEx(archive, name.encode('ascii'), 0, ctypes.byref(file)):
            raise ValueError('Archive member unavailable: ' + name)
        try:
            high = dword()
            size = storm.SFileGetFileSize(file, ctypes.byref(high))
            if high.value or size == 0xffffffff or size > 16 * 1024 * 1024:
                raise ValueError('Archive table exceeds supported bounded size')
            buffer, count = ctypes.create_string_buffer(size), dword()
            if not storm.SFileReadFile(file, buffer, size, ctypes.byref(count), None) or count.value != size:
                raise ValueError('Incomplete archive member: ' + name)
            return buffer.raw
        finally:
            storm.SFileCloseFile(file)

    try:
        yield read
        if sha256(path) != expected_sha256:
            raise ValueError('Archive changed during extraction')
    finally:
        storm.SFileCloseArchive(archive)


ENGLISH_PATCHSTRING_MEMBER = r'data\local\LNG\ENG\patchstring.tbl'


def read_english_patchstring(path, expected_sha256):
    """Read the exact English patchstring member from an authenticated MPQ."""
    with _authenticated_archive(path, expected_sha256) as read:
        return read(ENGLISH_PATCHSTRING_MEMBER)


def read_tables(path, expected_sha256):
    """Return original table bytes from one authenticated MPQ's internal listfile."""
    with _authenticated_archive(path, expected_sha256) as read:
        names = read('(listfile)').decode('ascii').splitlines()
        tables = {}
        for name in names:
            normalized = name.replace('\\', '/').lower()
            if not normalized.startswith('data/global/excel/') or not normalized.endswith('.txt'):
                continue
            if '..' in PurePosixPath(normalized).parts or normalized.count('/') != 3 or normalized in tables:
                raise ValueError('Unsafe or duplicate table member')
            tables[normalized] = read(name)
        if not tables:
            raise ValueError('Authenticated archive contains no listed source tables')
        return tables
