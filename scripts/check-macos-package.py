#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check bundled Mach-O targets, dependencies, architectures and signatures."""
import argparse
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

MACOS_MINIMUM = "15.0"
MACH_O_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
                b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce",
                b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
REQUIRED_FILES = (
    "Contents/MacOS/CrystalGoxel", "Contents/MacOS/goxel",
    "Contents/Frameworks/libglfw.3.dylib",
    "Contents/Resources/Bridge/crystal-bridge",
    "Contents/Resources/Bridge/LICENSE.TXT",
    "Contents/Resources/Bridge/THIRD-PARTY-NOTICES.TXT",
    "Contents/Resources/Goxel-LICENSE.txt",
    "Contents/Resources/GLFW-LICENSE.txt",
)


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def check(app):
    for name in REQUIRED_FILES:
        if not (app / name).is_file():
            raise ValueError(f"Missing package file: {name}")
    with (app / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("LSMinimumSystemVersion") != MACOS_MINIMUM:
        raise ValueError(f"The package must declare macOS {MACOS_MINIMUM}")
    binaries = []
    for path in app.rglob("*"):
        if path.is_file():
            with path.open("rb") as stream:
                if stream.read(4) in MACH_O_MAGIC:
                    binaries.append(path)
    bundled_names = {path.name for path in binaries}
    minimum = tuple(map(int, MACOS_MINIMUM.split(".")))
    for binary in binaries:
        if run("lipo", "-archs", str(binary)).strip() != "arm64":
            raise ValueError(f"{binary.name} is not an arm64 release binary")
        commands = run("otool", "-l", str(binary))
        targets = re.findall(r"\bminos ([0-9.]+)", commands)
        if not targets:
            # Older runtime binaries can use the legacy deployment load command
            targets = re.findall(r"LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", commands)
        if not targets:
            raise ValueError(f"No macOS deployment target in {binary.name}")
        if any(tuple(map(int, target.split(".")[:2])) > minimum
               for target in targets):
            raise ValueError(f"{binary.name} requires macOS newer than {MACOS_MINIMUM}: {targets}")
        for line in run("otool", "-L", str(binary)).splitlines()[1:]:
            dependency = line.strip().split(" (", 1)[0]
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            if dependency.startswith("@") and Path(dependency).name in bundled_names:
                continue
            raise ValueError(f"{binary.name} has an unbundled dependency: {dependency}")
    run("codesign", "--verify", "--deep", "--strict", str(app))
    print(f"Mac arm64 package dependencies, signatures and macOS {MACOS_MINIMUM} targets passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Requires macOS lipo, otool and codesign on PATH. No environment "
        "variables required. Exit: 0 success/help, 1 invalid package, "
        "2 usage, 3 missing dependency."))
    parser.add_argument("app", type=Path, help="Crystal Goxel.app directory")
    args = parser.parse_args()
    for dependency in ("lipo", "otool", "codesign"):
        if not shutil.which(dependency):
            print(f"Missing dependency: {dependency}", file=sys.stderr)
            return 3
    try:
        check(args.app)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
