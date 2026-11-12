#!/usr/bin/env python3
"""Build the Windows one-file Roguelike launcher using task-local build tools."""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
STORMLIB_SHA256 = "88eac929abe2c16503778f3fd44aa567670e9025349925ded8153682e0eb0167"
SOURCE_FILES = (
    "scripts/build_roguelike_launcher.py", "scripts/mod_archive.py", "scripts/mod_data.py",
    "scripts/mod_strings.py", "standalone/install.py", "standalone/launcher.py",
    "standalone/mod_data_plan.py", "standalone/releases.py",
    "standalone/licenses/Python.txt", "standalone/licenses/PyInstaller.txt",
    "standalone/licenses/OpenSSL.txt", "standalone/licenses/zlib.txt",
    "standalone/licenses/StormLib.txt", "standalone/licenses/StormLib-components.txt",
    "standalone/licenses/BH-AGPL.txt", "standalone/licenses/BH-Apache.txt",
    "standalone/licenses/Native-components.txt",
)


def source_hashes() -> dict[str, str]:
    result = {}
    for name in SOURCE_FILES:
        with (ROOT / name).open("rb") as stream:
            result[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


def build(stormlib: Path, output: Path) -> dict:
    if os.name != "nt" or struct.calcsize("P") != 8 or sys.version_info[:3] != (3, 12, 10):
        raise RuntimeError("Build with the verified Windows x64 Python 3.12.10 environment")
    if importlib.metadata.version("pyinstaller") != "6.16.0":
        raise RuntimeError("Build with PyInstaller 6.16.0")
    import tkinter
    if tkinter.Tcl().eval("info patchlevel") != "8.6.15":
        raise RuntimeError("Build with the verified Tcl/Tk 8.6.15 runtime")
    stormlib = Path(stormlib).resolve(strict=True)
    if not stormlib.is_file():
        raise ValueError("StormLib must be the verified 9.40 amd64 Release ANSI DLL")
    with stormlib.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != STORMLIB_SHA256:
            raise ValueError("StormLib must be the verified 9.40 amd64 Release ANSI DLL")
    licenses = ROOT / "standalone/licenses"
    sources = source_hashes()
    output = Path(output).absolute()
    output.mkdir(parents=False, exist_ok=False)
    try:
        with tempfile.TemporaryDirectory(prefix=".build-", dir=output) as temporary:
            temporary = Path(temporary)
            env = os.environ.copy()
            env["PYINSTALLER_CONFIG_DIR"] = str(temporary / "config")
            command = [sys.executable, "-m", "PyInstaller", "--onefile", "--windowed",
                       "--name", "PD2-Roguelike", "--paths", str(ROOT),
                       "--paths", str(ROOT / "scripts"), "--add-binary", str(stormlib) + ";.",
                       "--add-data", str(licenses) + ";standalone/licenses",
                       "--workpath", str(temporary / "build"), "--distpath", str(output),
                       "--specpath", str(temporary), str(ROOT / "standalone/launcher.py")]
            subprocess.run(command, env=env, check=True)
        executable = output / "PD2-Roguelike.exe"
        with executable.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if source_hashes() != sources:
            raise RuntimeError("Launcher sources changed during the build")
        receipt = {"schema_version": 1, "bytes": executable.stat().st_size, "sha256": digest,
                   "python": "3.12.10-x64", "tcl": "8.6.15", "pyinstaller": "6.16.0",
                   "stormlib_sha256": STORMLIB_SHA256, "sources": sources}
        (output / "launcher-receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return {"path": str(executable), **receipt}
    except BaseException:
        if output.exists() and not any(output.iterdir()):
            output.rmdir()
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stormlib", required=True, type=Path, help="Verified StormLib.dll")
    parser.add_argument("--output", required=True, type=Path, help="New output directory")
    args = parser.parse_args()
    print(json.dumps(build(args.stormlib, args.output), indent=2))


if __name__ == "__main__":
    main()
