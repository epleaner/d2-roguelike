#!/usr/bin/env python3
"""Prepare the approved launcher icon from a private, authenticated D2 archive."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_roguelike_launcher as launcher_build
import mod_archive
from standalone.mod_data_plan import CLASSIC_SHA256

MEMBERS = {
    "pponhth.cof": r"data\global\objects\PP\COF\PPONHTH.COF",
    "pphdlitonhth.dcc": r"data\global\objects\PP\HD\pphdlitonhth.dcc",
    "pptrlitonhth.dcc": r"data\global\objects\PP\TR\pptrlitonhth.dcc",
    "pal.dat": r"data\global\palette\ACT1\pal.dat",
}


def prepare(classic_archive: Path, output: Path) -> dict[str, str]:
    output = Path(output).absolute().resolve(strict=False)
    if output.is_relative_to(ROOT) or output.exists():
        raise ValueError("Use a new private artwork directory outside the checkout")
    with tempfile.TemporaryDirectory(prefix="d2-portal-art-") as temporary:
        temporary = Path(temporary)
        with mod_archive._authenticated_archive(classic_archive, CLASSIC_SHA256) as read:
            for name, member in MEMBERS.items():
                (temporary / name).write_bytes(read(member))
        frame = temporary / "portal.png"
        subprocess.run(["go", "run", "-mod=readonly", ".", str(temporary), str(frame)],
                       cwd=ROOT / "scripts/portal_art", check=True)
        artwork = temporary / "artwork"
        artwork.mkdir()
        png = artwork / "red-portal.png"
        ico = artwork / "red-portal.ico"
        subprocess.run(["magick", str(frame), "-filter", "point", "-resize", "232x232",
                        "-background", "none", "-gravity", "center", "-extent", "256x256",
                        "-strip", "PNG32:" + str(png)], check=True)
        subprocess.run(["magick", str(png), "-filter", "point", "-define",
                        "icon:auto-resize=256,128,64,48,32,24,16", str(ico)], check=True)
        hashes = launcher_build.artwork_hashes(artwork)
        output.mkdir()
        try:
            for name in hashes:
                shutil.copyfile(artwork / name, output / name)
        except BaseException:
            shutil.rmtree(output)
            raise
    return hashes


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--classic-archive", required=True, type=Path, help="Authenticated original d2data.mpq")
    parser.add_argument("--output", required=True, type=Path, help="New private artwork directory")
    args = parser.parse_args()
    print(prepare(args.classic_archive, args.output))


if __name__ == "__main__":
    main()
