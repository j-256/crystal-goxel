#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check executable imports and required portable package files."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

# These are Windows OS libraries, not dependencies supplied by MSYS2
SYSTEM_LIBRARIES = {
    "advapi32.dll", "bcrypt.dll", "comdlg32.dll", "crypt32.dll",
    "dbghelp.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    "mscoree.dll", "msvcrt.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
    "opengl32.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll",
    "shell32.dll", "shlwapi.dll", "sspicli.dll", "ucrtbase.dll", "user32.dll",
    "userenv.dll", "version.dll", "winmm.dll", "wintrust.dll",
    "ws2_32.dll", "wtsapi32.dll",
}
REQUIRED_FILES = (
    "CrystalGoxel.exe", "Bridge/crystal-bridge.exe", "Bridge/coreclr.dll",
    "Bridge/LICENSE.TXT", "Bridge/THIRD-PARTY-NOTICES.TXT",
    "licenses/Goxel-LICENSE.txt", "licenses/GLFW-LICENSE.txt",
    "licenses/TRE-LICENSE.txt", "README.md", "CRYSTAL_BRIDGE.md",
    "doc/WINDOWS_TESTING.md",
    "licenses/GCC-COPYING3.txt", "licenses/GCC-COPYING.RUNTIME.txt",
    "licenses/MINGW-LICENSE.txt",
)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Requires objdump on PATH. No environment variables required. "
               "Prints a success result to stdout and errors to stderr. "
               "Exit: 0 success/help, 1 invalid package, 2 usage, "
               "3 missing dependency.")
    parser.add_argument("package", type=Path, help="portable package directory")
    args = parser.parse_args()
    if not shutil.which("objdump"):
        print("Missing dependency: objdump", file=sys.stderr)
        return 3
    try:
        for name in REQUIRED_FILES:
            if not (args.package / name).is_file():
                raise ValueError(f"Missing package file: {name}")
        for binary in args.package.rglob("*"):
            if binary.suffix.lower() not in {".exe", ".dll"}:
                continue
            # Managed DLLs without native imports are valid PE files too
            result = subprocess.run(["objdump", "-p", str(binary)],
                                    check=True, capture_output=True, text=True)
            siblings = {p.name.lower() for p in binary.parent.iterdir()}
            for imported in re.findall(r"DLL Name:\s*(\S+)", result.stdout):
                name = imported.lower()
                if (name in SYSTEM_LIBRARIES or name.startswith(
                        ("api-ms-win-", "ext-ms-win-")) or name in siblings):
                    continue
                raise ValueError(f"{binary.name} needs unbundled {imported}")
        print("Windows package dependencies and notices passed")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
