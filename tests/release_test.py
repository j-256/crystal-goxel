# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise release provenance, integrity and private-runner policy."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import yaml

PROJECT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("release", PROJECT / "scripts/crystal-release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / "repo"
        self.repo.mkdir()
        self.root_patch = patch.object(release, "ROOT", self.repo)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        (self.repo / "CRYSTAL_VERSION").write_text("0.1.0-preview.1\n")
        for name in release.DOCUMENTS:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name + "\n")
        (self.repo / "doc/RELEASE_NOTES.md").write_text("Preview @VERSION@\n")
        self.git("init", "-q")
        self.git("add", "CRYSTAL_VERSION", *release.DOCUMENTS, "doc/RELEASE_NOTES.md")
        self.commit()
        self.inputs = self.base / "inputs"
        self.inputs.mkdir()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True).strip()

    def commit(self):
        self.git("-c", "user.name=Release Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "core.hooksPath=/dev/null", "commit", "-q", "-m", "Fixture")

    def package(self, platform):
        package = self.base / release.PACKAGE_ROOTS[platform]
        for name in release.REQUIRED_MEMBERS[platform]:
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic fixture\n")
            path.chmod(0o755)
        release.stamp(package, platform)
        return package

    def archives(self):
        for platform in release.PLATFORMS:
            package = self.package(platform)
            with patch.object(release, "check_package"):
                release.archive(package, platform, self.inputs / release.package_name(platform))

    def test_release_round_trip_and_matching_source(self):
        self.archives()
        output = self.base / "release"
        release.assemble(self.inputs, output, "crystal-v0.1.0-preview.1")
        release.verify(output)
        with zipfile.ZipFile(output / "Crystal-Goxel-0.1.0-preview.1-source.zip") as stream:
            self.assertEqual(stream.comment.decode(), self.git("rev-parse", "HEAD"))
            self.assertFalse(any(".git/" in name for name in stream.namelist()))
        with zipfile.ZipFile(output / release.package_name("macos-arm64")) as stream:
            mode = stream.getinfo("Crystal Goxel.app/Contents/MacOS/CrystalGoxel").external_attr >> 16
            self.assertTrue(mode & 0o111)

    def test_stable_release_notes_keep_platform_limits(self):
        (self.repo / "CRYSTAL_VERSION").write_text("0.1.0\n")
        notes = (PROJECT / "doc/RELEASE_NOTES.md").read_text()
        (self.repo / "doc/RELEASE_NOTES.md").write_text(notes)
        self.git("add", "CRYSTAL_VERSION", "doc/RELEASE_NOTES.md")
        self.commit()
        self.archives()
        output = self.base / "stable"
        release.assemble(self.inputs, output, "crystal-v0.1.0")
        rendered = (output / "release-notes.md").read_text()
        self.assertEqual(rendered.splitlines()[0], "# Crystal Goxel 0.1.0")
        self.assertIn("Windows graphical authoring", rendered)
        self.assertIn("unverified", rendered)
        self.assertIn("not notarized", rendered)
        self.assertNotIn("@VERSION@", rendered)

    def test_dirty_build_and_later_revision_are_rejected(self):
        package = self.package("windows-x64")
        (self.repo / "README.md").write_text("Changed\n")
        metadata = json.loads(release.manifest_path(package, "windows-x64").read_text())
        with self.assertRaises(release.Precondition):
            release.validate_metadata(metadata)
        self.git("add", "README.md")
        self.commit()
        with self.assertRaises(release.Precondition):
            release.validate_metadata(metadata)

    def test_stamp_keeps_dirty_build_explicit(self):
        (self.repo / "uncommitted.txt").write_text("new source\n")
        package = self.package("windows-x64")
        metadata = json.loads(release.manifest_path(package, "windows-x64").read_text())
        self.assertTrue(metadata["dirty"])
        with self.assertRaises(release.Precondition):
            release.validate_metadata(metadata)

    def test_checksum_tampering_and_missing_platform(self):
        self.archives()
        output = self.base / "release"
        release.assemble(self.inputs, output)
        (output / "release-notes.md").write_text("modified\n")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            release.verify(output)
        (self.inputs / release.package_name("macos-arm64")).unlink()
        with self.assertRaises(release.Precondition):
            release.assemble(self.inputs, self.base / "incomplete")

    def test_symlinks_and_private_context_are_rejected(self):
        package = self.package("windows-x64")
        link = package / "linked-file"
        link.symlink_to(self.repo / "CRYSTAL_VERSION")
        with patch.object(release, "check_package"), self.assertRaises(release.Precondition):
            release.archive(package, "windows-x64", self.base / "linked.zip")
        link.unlink()
        (package / "world.json").write_text("private reference\n")
        with patch.object(release, "check_package"), self.assertRaises(release.Precondition):
            release.archive(package, "windows-x64", self.base / "private.zip")
        (package / "world.json").unlink()
        (package / "helper.pdb").write_text("private build path\n")
        with patch.object(release, "check_package"), self.assertRaises(release.Precondition):
            release.archive(package, "windows-x64", self.base / "symbols.zip")

    def test_unsafe_zip_path_is_rejected(self):
        path = self.base / "unsafe.zip"
        with zipfile.ZipFile(path, "w") as stream:
            stream.writestr("../outside", "unsafe")
        with self.assertRaisesRegex(release.Precondition, "Unsafe archive path"):
            release.inspect_archive(path)

    def test_wrong_tag_and_existing_output_are_rejected(self):
        with self.assertRaises(release.Precondition):
            release.version("crystal-v9.9.9")
        output = self.base / "existing"
        output.mkdir()
        with self.assertRaises(release.Precondition):
            release.assemble(self.inputs, output)

    def prepare_version_checks(self, passes=True):
        checks = self.repo / "tests/release_test.py"
        checks.parent.mkdir()
        checks.write_text("import unittest\nclass ReleaseGate(unittest.TestCase):\n"
                          f"    def test_contract(self): self.assertTrue({passes!r})\n")
        self.git("add", "tests/release_test.py")
        self.commit()

    def test_version_update_runs_checks_before_changing_source(self):
        self.prepare_version_checks()
        setter = getattr(release, "set_version", None)
        self.assertTrue(callable(setter), "Missing guarded version setter")
        setter("0.1.0")
        self.assertEqual(release.version("crystal-v0.1.0"), "0.1.0")
        self.assertEqual(self.git("diff", "--name-only"), "CRYSTAL_VERSION")
        self.assertEqual(self.git("tag", "--list"), "")

    def test_version_update_rejects_dirty_and_detached_checkouts(self):
        self.prepare_version_checks()
        (self.repo / "untracked.txt").write_text("unfinished work\n")
        with self.assertRaisesRegex(release.Precondition, "clean"):
            release.set_version("0.1.0")
        (self.repo / "untracked.txt").unlink()
        self.git("checkout", "--detach", "-q")
        with self.assertRaisesRegex(release.Precondition, "branch"):
            release.set_version("0.1.0")
        self.assertEqual(release.version(), "0.1.0-preview.1")

    def test_version_update_rejects_invalid_nonadvancing_and_tagged_versions(self):
        self.prepare_version_checks()
        for value in ("0.1.0-preview.1", "0.0.9", "0.1.0-preview.0", "01.0.0", "garbage"):
            with self.subTest(version=value), self.assertRaises(release.Precondition):
                release.set_version(value)
        self.git("tag", "crystal-v0.1.0")
        with self.assertRaisesRegex(release.Precondition, "tag"):
            release.set_version("0.1.0")
        self.assertEqual(release.version(), "0.1.0-preview.1")

    def test_version_update_preserves_source_when_release_checks_fail(self):
        self.prepare_version_checks(passes=False)
        with self.assertRaises(subprocess.CalledProcessError):
            release.set_version("0.1.0")
        self.assertEqual(release.version(), "0.1.0-preview.1")

    def test_version_update_rechecks_tag_after_release_checks(self):
        self.prepare_version_checks()
        checks = self.repo / "tests/release_test.py"
        checks.write_text("import subprocess\nimport unittest\n"
                          "class ReleaseGate(unittest.TestCase):\n"
                          "    def test_contract(self):\n"
                          "        subprocess.run(['git', 'tag', 'crystal-v0.1.0'], check=True)\n")
        self.git("add", "tests/release_test.py")
        self.commit()
        with self.assertRaisesRegex(release.Precondition, "tag"):
            release.set_version("0.1.0")
        self.assertEqual(release.version(), "0.1.0-preview.1")

    def test_version_update_orders_preview_and_stable_versions(self):
        self.prepare_version_checks()
        release.set_version("0.1.0-preview.2")
        self.git("add", "CRYSTAL_VERSION")
        self.commit()
        release.set_version("0.1.0")
        self.git("add", "CRYSTAL_VERSION")
        self.commit()
        with self.assertRaises(release.Precondition):
            release.set_version("0.1.0-preview.3")
        release.set_version("0.1.1-preview.1")
        self.assertEqual(release.version(), "0.1.1-preview.1")


class ReleaseCLI(unittest.TestCase):
    def test_numeric_version_and_tag_remain_composable(self):
        value = (PROJECT / "CRYSTAL_VERSION").read_text().strip()
        result = subprocess.run([sys.executable, str(PROJECT / "scripts/crystal-release.py"),
                                 "version", "--numeric", "--tag", f"crystal-v{value}"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), value.split("-", 1)[0])
        self.assertEqual(result.stderr, "")


class RunnerPolicy(unittest.TestCase):
    def test_every_job_requires_public_visibility_and_standard_runner(self):
        allowed = {"windows-2022", "macos-15", "ubuntu-24.04"}
        paths = sorted((PROJECT / ".github/workflows").glob("*.y*ml"))
        self.assertTrue(paths)
        public_guard = "${{ github.event.repository.private == false }}"
        tag_guard = ("${{ github.event.repository.private == false && "
                     "github.event_name == 'push' && "
                     "startsWith(github.ref, 'refs/tags/crystal-v') }}")
        for path in paths:
            workflow = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
            self.assertEqual(workflow["permissions"], {"contents": "read"})
            for name, job in workflow["jobs"].items():
                with self.subTest(workflow=path.name, job=name):
                    condition = job.get("if", "")
                    self.assertIn(job.get("runs-on"), allowed)
                    if job.get("permissions", {}).get("contents") == "write":
                        self.assertEqual(condition, tag_guard)
                        self.assertEqual(set(job["needs"]), {"windows_x64", "macos_arm64", "release_contract"})
                    else:
                        self.assertEqual(condition, public_guard)


if __name__ == "__main__":
    unittest.main()
