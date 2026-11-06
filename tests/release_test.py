"""Check that release packaging accepts runtime files but rejects installation data."""

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "release", Path(__file__).resolve().parents[1] / "scripts/release.py"
)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseManifestTests(unittest.TestCase):
    def package(self, root, files):
        manifest = []
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            manifest.append(f"{hashlib.sha256(content).hexdigest()}  {name}\n")
        (root / "SHA256SUMS").write_text("".join(manifest))

    def test_windows_runtime_database(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.package(root, {"bin/SECURITY5.FDB": b"runtime fixture"})
            self.assertTrue(release.verify(root))

    def test_installation_files_are_rejected(self):
        for name in ("data/mail.fdb", "config/local.json", "bin/other.fdb", "tls.key"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.package(root, {name: b"private fixture"})
                with self.assertRaises(ValueError):
                    release.verify(root)

    def test_modified_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.package(root, {"bin/program": b"original"})
            (root / "bin/program").write_bytes(b"modified")
            with self.assertRaises(ValueError):
                release.verify(root)


if __name__ == "__main__":
    unittest.main()
