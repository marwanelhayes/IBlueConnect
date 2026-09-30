"""Assemble/extract real Debian archives without installing or starting anything."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PACKAGING = Path(__file__).resolve().parent


class PackageTest(unittest.TestCase):
    def test_native_package_contains_runtime_and_has_no_service_hooks(self):
        version = subprocess.check_output(
            ["sh", "-c", '. /etc/os-release; printf "%s" "$VERSION_ID"'], text=True)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            packaging = root / "packaging"
            packaging.mkdir()
            shutil.copy2(PACKAGING / "build-deb.sh", packaging)
            for name in ("ofono-enable.sh", "ofono-rollback.sh", "rnnoise.sh",
                         "first-run.sh", "iblueconnect-launcher.sh", "setup-user.py"):
                (packaging / name).write_text("#!/bin/sh\nexit 0\n")
            (packaging / "systemd").mkdir()
            (packaging / "systemd/iblueconnectd-package.service").write_text(
                "[Service]\nExecStart=/usr/lib/iblueconnect/iblueconnectd\n")
            (packaging / "desktop").mkdir()
            (packaging / "desktop/org.iblueconnect.IBlueConnect.desktop").write_text(
                "[Desktop Entry]\nType=Application\nExec=iblueconnect\n")
            (root / "Cargo.toml").write_text('[package]\nversion = "0.1.0"\n')
            licenses = root / "target/rnnoise/noise-suppression-for-voice-1.10"
            (licenses / "external/rnnoise").mkdir(parents=True)
            (licenses / "LICENSE").write_text("Upstream license\n")
            (licenses / "external/rnnoise/COPYING").write_text("RNNoise license\n")
            result = subprocess.run(["sh", str(packaging / "build-deb.sh"), version,
                                     "/bin/true", "/bin/true", "/bin/true"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            release = root / "target/releases"
            archive, = release.glob("*.deb")
            control = subprocess.check_output(["dpkg-deb", "-f", str(archive)], text=True)
            self.assertIn("Architecture: amd64", control)
            self.assertIn(f"X-IBlueConnect-Ubuntu: {version}", control)
            self.assertIn("libc6 (>=", control)
            self.assertIn("ofono", control)
            self.assertIn("gnome-session-canberra", control)
            self.assertIn("libcanberra-pulse", control)
            self.assertNotIn("libcanberra-gtk3-bin", control)
            extracted = root / "extracted"
            subprocess.run(["dpkg-deb", "-R", str(archive), str(extracted)], check=True)
            for item in ("usr/bin/iblueconnect", "usr/lib/iblueconnect/iblueconnect",
                         "usr/lib/iblueconnect/iblueconnectd",
                         "usr/lib/iblueconnect/librnnoise_ladspa.so",
                         "usr/lib/iblueconnect/first-run.sh",
                         "usr/lib/iblueconnect/setup-user.py",
                         "usr/lib/iblueconnect/setup-venv/bin/python3",
                         "usr/lib/systemd/user/iblueconnectd.service",
                         "usr/share/doc/iblueconnect/RNNoise-COPYING"):
                self.assertTrue((extracted / item).is_file(), item)
            self.assertEqual((extracted / "usr/lib/iblueconnect/ubuntu-version").read_text().strip(), version)
            for hook in ("preinst", "postinst", "prerm", "postrm"):
                self.assertFalse((extracted / "DEBIAN" / hook).exists())
            for relative in ("DEBIAN/control", "usr/bin/iblueconnect",
                             "usr/lib/systemd/user/iblueconnectd.service",
                             "usr/lib/iblueconnect/setup-venv/pyvenv.cfg"):
                self.assertNotIn("/home/", (extracted / relative).read_text())
                self.assertNotIn(str(root), (extracted / relative).read_text())
            interpreter = extracted / "usr/lib/iblueconnect/setup-venv/bin/python3"
            prefixes = json.loads(subprocess.check_output([str(interpreter), "-c",
                "import json,sys; print(json.dumps([sys.prefix,sys.base_prefix]))"], text=True))
            self.assertTrue(prefixes[0].endswith("/setup-venv"))
            self.assertNotEqual(*prefixes)
            manifest = json.loads((release / f"ubuntu-{version}-amd64.json").read_text())
            self.assertEqual(manifest["download_bytes"], archive.stat().st_size)
            self.assertEqual(manifest["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertGreater(manifest["installed_kib"], 0)
            wrong = "24.04" if version == "26.04" else "26.04"
            result = subprocess.run(["sh", str(packaging / "build-deb.sh"), wrong,
                                     "/bin/true", "/bin/true", "/bin/true"],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("matching Ubuntu", result.stderr)


if __name__ == "__main__":
    unittest.main()
