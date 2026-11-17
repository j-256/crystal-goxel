# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise packaged helper startup failures without native game resources."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

PROJECT = Path(__file__).resolve().parent.parent


@unittest.skipUnless(shutil.which("dotnet"), "Requires .NET SDK 10")
class NativeStartup(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.bridge = root / "bridge"
        self.bridge.mkdir()
        for source in (PROJECT / "bridge").iterdir():
            if source.suffix in {".cs", ".csproj", ".json"}:
                shutil.copyfile(source, self.bridge / source.name)
        shutil.copyfile(PROJECT / "CRYSTAL_VERSION", root / "CRYSTAL_VERSION")

    def assert_startup_error(self, expected):
        build = subprocess.run(["dotnet", "build", str(self.bridge / "CrystalBridge.csproj"),
                                "-c", "Release", "-p:RestoreLockedMode=true", "--nologo"],
                               capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run(["dotnet", str(self.bridge / "bin/Release/net10.0/crystal-bridge.dll"), "--help"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertRegex(result.stderr, rf"crystal-bridge operation=help result=error exit={expected} elapsedMs=\d+ id=[a-f0-9]+ message=")
        self.assertNotIn("Unhandled exception", result.stderr)

    def test_missing_catalog_reports_structured_startup_error(self):
        project = self.bridge / "CrystalBridge.csproj"
        tree = ET.parse(project)
        for group in tree.getroot().findall("ItemGroup"):
            for item in list(group):
                if item.tag == "EmbeddedResource" and item.get("Include") == "native-builds.json":
                    group.remove(item)
        tree.write(project, encoding="unicode")
        self.assert_startup_error(2)

    def test_malformed_catalog_reports_structured_startup_error(self):
        (self.bridge / "native-builds.json").write_text("{invalid JSON")
        self.assert_startup_error(1)


if __name__ == "__main__":
    unittest.main()
