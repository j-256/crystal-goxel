#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stamp builds and prepare checked preview assets without publishing them."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
PLATFORMS = ("windows-x64", "macos-arm64")
DOCUMENTS = ("README.md", "CRYSTAL_BRIDGE.md", "COPYING", "CONTRIBUTING.md", "AGENTS.md",
             "doc/WINDOWS_TESTING.md", "doc/RELEASING.md", "doc/RELEASE_NOTES.md",
             "doc/UPSTREAM_README.md")
VERSION_PATTERN = r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-preview\.[1-9]\d*)?"
PACKAGE_ROOTS = {"windows-x64": "Crystal Goxel Windows", "macos-arm64": "Crystal Goxel.app"}
REQUIRED_MEMBERS = {
    "windows-x64": ("CrystalGoxel.exe", "Bridge/crystal-bridge.exe", "Bridge/coreclr.dll",
                    "Bridge/LICENSE.TXT", "Bridge/THIRD-PARTY-NOTICES.TXT",
                    "licenses/Goxel-LICENSE.txt", "licenses/GLFW-LICENSE.txt", "README.md"),
    "macos-arm64": ("Contents/Info.plist", "Contents/MacOS/CrystalGoxel", "Contents/MacOS/goxel",
                    "Contents/Resources/Bridge/crystal-bridge", "Contents/Resources/Bridge/libcoreclr.dylib",
                    "Contents/Resources/Bridge/LICENSE.TXT", "Contents/Resources/Bridge/THIRD-PARTY-NOTICES.TXT",
                    "Contents/Resources/Goxel-LICENSE.txt", "Contents/Resources/GLFW-LICENSE.txt"),
}


class Precondition(ValueError):
    pass


class MissingDependency(RuntimeError):
    pass


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def version(tag=None):
    value = (ROOT / "CRYSTAL_VERSION").read_text().strip()
    if not re.fullmatch(VERSION_PATTERN, value):
        raise Precondition("CRYSTAL_VERSION must be X.Y.Z or X.Y.Z-preview.N")
    if tag is not None and tag != f"crystal-v{value}":
        raise Precondition(f"Release tag must be crystal-v{value}")
    return value


def source():
    return {"sourceCommit": git("rev-parse", "HEAD"),
            "sourceTree": git("rev-parse", "HEAD^{tree}"),
            "dirty": bool(git("status", "--porcelain", "--untracked-files=normal"))}


def manifest_path(package, platform):
    if platform == "macos-arm64":
        return package / "Contents/Resources/release.json"
    return package / "release.json"


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def stamp(package, platform):
    if not package.is_dir():
        raise Precondition(f"Missing built package: {package}")
    metadata = {"schemaVersion": 1, "version": version(), "platform": platform,
                "minimumOS": "macOS 15.0" if platform == "macos-arm64" else "Windows 11",
                "signing": "ad-hoc, not notarized" if platform == "macos-arm64" else "unsigned",
                **source()}
    write_json(manifest_path(package, platform), metadata)
    return metadata


def validate_metadata(metadata):
    if metadata.get("schemaVersion") != 1 or metadata.get("platform") not in PLATFORMS:
        raise Precondition("Unsupported package provenance schema or platform")
    if metadata.get("version") != version() or metadata.get("dirty") is not False:
        raise Precondition("Release packages must be clean builds of CRYSTAL_VERSION")
    expected = source()
    if expected["dirty"]:
        raise Precondition("Commit all source changes before preparing a release")
    for key in ("sourceCommit", "sourceTree"):
        if metadata.get(key) != expected[key]:
            raise Precondition(f"Package {key} differs from this checkout; rebuild it")


def package_name(platform):
    return f"Crystal-Goxel-{version()}-{platform}.zip"


def check_package(package, platform):
    checker = ROOT / "scripts" / ("check-macos-package.py" if platform == "macos-arm64"
                                  else "check-windows-package.py")
    result = subprocess.run([sys.executable, str(checker), str(package)],
                            stdout=sys.stderr)
    if result.returncode == 3:
        raise MissingDependency("Missing dependency for the native package checker")
    if result.returncode:
        raise ValueError("Native package verification failed")


def archive(package, platform, output):
    metadata = json.loads(manifest_path(package, platform).read_text())
    if metadata.get("platform") != platform:
        raise Precondition("Package platform does not match the archive platform")
    validate_metadata(metadata)
    check_package(package, platform)
    if output.exists():
        raise Precondition(f"Output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Refuse arbitrary links and user reference data rather than dereferencing them into a release
    files = []
    for path in sorted(package.rglob("*")):
        if path.is_symlink():
            raise Precondition(f"Release packages cannot contain symlinks: {path.name}")
        if path.suffix.lower() == ".pdb":
            raise Precondition(f"Remove helper debug symbols with private build paths: {path.name}")
        if path.name in {"world.json", "locations.json"} or path.suffix.lower() == ".gox":
            raise Precondition(f"Private authoring data in package: {path.name}")
        if path.is_file() and path.name != ".DS_Store":
            files.append((path, f"{PACKAGE_ROOTS[platform]}/{path.relative_to(package).as_posix()}"))
    if platform == "macos-arm64":
        files.extend((ROOT / name, name) for name in DOCUMENTS)
        files.extend((path, f"licenses/{path.name}") for path in sorted((ROOT / "licenses").glob("*")) if path.is_file())
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        candidate = Path(temporary) / "candidate.zip"
        with zipfile.ZipFile(candidate, "w", zipfile.ZIP_DEFLATED) as stream:
            for path, name in files:
                stream.write(path, name)
        inspect_archive(candidate)
        candidate.rename(output)


def inspect_archive(path):
    with zipfile.ZipFile(path) as stream:
        names = stream.namelist()
        if len(names) != len(set(names)):
            raise Precondition(f"Duplicate archive entries: {path.name}")
        for entry in stream.infolist():
            name = PurePosixPath(entry.filename)
            if name.suffix.lower() == ".pdb":
                raise Precondition(f"Archive contains private build symbols: {entry.filename}")
            if name.is_absolute() or ".." in name.parts or "\\" in entry.filename:
                raise Precondition(f"Unsafe archive path: {entry.filename}")
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise Precondition(f"Archive contains a symlink: {entry.filename}")
        bad = stream.testzip()
        if bad:
            raise ValueError(f"Corrupt ZIP member: {bad}")
        manifests = [name for name in names if name.endswith("/release.json")]
        if len(manifests) != 1:
            raise Precondition(f"Expected one package manifest in {path.name}")
        metadata = json.loads(stream.read(manifests[0]))
        validate_metadata(metadata)
        platform = metadata["platform"]
        required = {f"{PACKAGE_ROOTS[platform]}/{name}" for name in REQUIRED_MEMBERS[platform]}
        if not required.issubset(names):
            raise Precondition(f"Archive is missing packaged editor, helper or notices: {path.name}")
        if platform == "macos-arm64":
            for name in ("Contents/MacOS/goxel", "Contents/MacOS/CrystalGoxel", "Contents/Resources/Bridge/crystal-bridge"):
                if not stream.getinfo(f"{PACKAGE_ROOTS[platform]}/{name}").external_attr >> 16 & 0o111:
                    raise Precondition(f"Mac executable permissions missing: {name}")
        return metadata


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assemble(inputs, output, tag=None):
    version(tag)
    if source()["dirty"]:
        raise Precondition("Commit all source changes before assembling a release")
    if output.exists():
        raise Precondition(f"Output already exists: {output}")
    archives = {}
    for path in sorted(inputs.glob("*.zip")):
        metadata = inspect_archive(path)
        platform = metadata["platform"]
        if platform in archives:
            raise Precondition(f"Duplicate platform archive: {platform}")
        if path.name != package_name(platform):
            raise Precondition(f"Unexpected asset name: {path.name}")
        archives[platform] = path
    if set(archives) != set(PLATFORMS):
        raise Precondition("Both Windows x64 and Mac arm64 archives are required")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        prepared = Path(temporary) / "release"
        prepared.mkdir()
        for path in archives.values():
            shutil.copyfile(path, prepared / path.name)
        source_name = f"Crystal-Goxel-{version()}-source.zip"
        subprocess.run(["git", "-C", str(ROOT), "archive", "--format=zip",
                        f"--prefix=Crystal-Goxel-{version()}-source/",
                        "--output", str(prepared / source_name), "HEAD"], check=True)
        write_json(prepared / "release.json", {"schemaVersion": 1, "version": version(),
                   "tag": f"crystal-v{version()}", "platforms": list(PLATFORMS), **source()})
        notes = (ROOT / "doc/RELEASE_NOTES.md").read_text().replace("@VERSION@", version())
        (prepared / "release-notes.md").write_text(notes)
        sums = "".join(f"{digest(path)}  {path.name}\n" for path in sorted(prepared.iterdir()))
        (prepared / "SHA256SUMS").write_text(sums)
        verify(prepared)
        prepared.rename(output)


def verify(directory):
    metadata = json.loads((directory / "release.json").read_text())
    if metadata.get("version") != version() or metadata.get("dirty") is not False:
        raise Precondition("Release manifest differs from CRYSTAL_VERSION or is dirty")
    if metadata.get("sourceCommit") != source()["sourceCommit"] or metadata.get("sourceTree") != source()["sourceTree"]:
        raise Precondition("Release manifest differs from the checkout revision")
    expected_names = {package_name(platform) for platform in PLATFORMS}
    source_name = f"Crystal-Goxel-{version()}-source.zip"
    expected_names |= {source_name, "release.json", "release-notes.md"}
    listed = set()
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if not match or match[2] not in expected_names or match[2] in listed:
            raise Precondition("Invalid or duplicate checksum entry")
        listed.add(match[2])
        if digest(directory / match[2]) != match[1]:
            raise ValueError(f"Checksum mismatch: {match[2]}")
    if listed != expected_names:
        raise Precondition("Release checksums do not cover every expected asset")
    if {path.name for path in directory.iterdir()} != expected_names | {"SHA256SUMS"}:
        raise Precondition("Release directory contains unexpected files")
    for platform in PLATFORMS:
        if inspect_archive(directory / package_name(platform))["platform"] != platform:
            raise Precondition("Release archive platform does not match its name")
    with zipfile.ZipFile(directory / source_name) as stream:
        prefix = f"Crystal-Goxel-{version()}-source/"
        if stream.comment.decode() != metadata["sourceCommit"]:
            raise Precondition("Source archive commit does not match the release")
        if stream.testzip() or stream.read(prefix + "CRYSTAL_VERSION").decode().strip() != version():
            raise ValueError("Source archive is corrupt or has a different version")
        if not stream.read(prefix + "COPYING"):
            raise Precondition("Source archive must include its license")


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Requires Python 3.11+, and Git for stamp/archive/assemble/verify. "
        "archive also requires native package checker dependencies: objdump "
        "for Windows; macOS lipo, otool and codesign for Mac. No environment "
        "variables required. CRYSTAL_VERSION is X.Y.Z or X.Y.Z-preview.N; tag names "
        "are crystal-vVERSION. Packages contain schema-1 release.json with "
        "version, platform, sourceCommit, sourceTree and dirty. Output paths "
        "must not exist. Exit: 0 success/help, 1 verification/runtime failure, "
        "2 usage/precondition, 3 missing dependency."))
    commands = parser.add_subparsers(dest="command", required=True)
    value = commands.add_parser("version", help="print CRYSTAL_VERSION or its numeric core")
    value.add_argument("--numeric", action="store_true")
    value.add_argument("--tag", help="require a tag matching CRYSTAL_VERSION")
    for name in ("stamp", "archive"):
        command = commands.add_parser(name, help=f"{name} a built package")
        command.add_argument("package", type=Path)
        command.add_argument("--platform", required=True, choices=PLATFORMS)
        if name == "archive":
            command.add_argument("--output", required=True, type=Path)
    command = commands.add_parser("assemble", help="combine platform ZIPs, source, notes and checksums")
    command.add_argument("inputs", type=Path, help="folder containing both versioned platform ZIPs")
    command.add_argument("--output", required=True, type=Path)
    command.add_argument("--tag", help="require a tag matching CRYSTAL_VERSION")
    command = commands.add_parser("verify", help="check a prepared release and every checksum")
    command.add_argument("directory", type=Path)
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        print("Requires Python 3.11 or later", file=sys.stderr)
        return 3
    if args.command != "version" and not shutil.which("git"):
        print("Missing dependency: git", file=sys.stderr)
        return 3
    try:
        if args.command == "version":
            value = version(args.tag)
            print(value.split("-", 1)[0] if args.numeric else value)
        elif args.command == "stamp":
            stamp(args.package, args.platform)
            print(manifest_path(args.package, args.platform))
        elif args.command == "archive":
            archive(args.package, args.platform, args.output)
            print(args.output)
        elif args.command == "assemble":
            assemble(args.inputs, args.output, args.tag)
            print(args.output)
        else:
            verify(args.directory)
            print("Release archives, source and checksums passed")
        return 0
    except Precondition as error:
        print(str(error), file=sys.stderr)
        return 2
    except MissingDependency as error:
        print(str(error), file=sys.stderr)
        return 3
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
