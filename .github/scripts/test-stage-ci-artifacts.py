"""Test the verified legacy package handoff without rebuilding OBS."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("stage", Path(__file__).with_name("stage-ci-artifacts.py"))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

class Handoff(unittest.TestCase):
    def test_preserves_packages_and_rejects_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            old = root / "framerot-ci" / "artifacts"
            old.mkdir(parents=True)
            binary = old / "framerot-linux-x86_64-obs32.tar.gz"
            source = old / "framerot-source.tar.gz"
            binary.write_bytes(b"verified binary")
            source.write_bytes(b"verified source")
            def digest(p):
                return hashlib.sha256(p.read_bytes()).hexdigest()
            (old / "ARCHIVE-SHA256SUMS").write_text("".join(
                f"{digest(p)}  ./{p.name}\n" for p in (binary, source)))
            payload = "a" * 64 + "  framerot/bin/64bit/framerot.so\n"
            (old / "SHA256SUMS").write_text(payload)
            with self.assertRaises(ValueError):
                stage.prepare(root, "invalid-name.tar.gz")
            source.write_bytes(b"corrupted")
            with self.assertRaises(ValueError):
                stage.prepare(root, stage.BINARY)
            self.assertFalse((root / "plugin-ci").exists())
            source.write_bytes(b"verified source")
            stage.prepare(root, stage.BINARY)
            self.assertEqual((root / "plugin-ci" / stage.BINARY).read_bytes(), binary.read_bytes())
            self.assertEqual((root / "plugin-ci" / "SHA256SUMS").read_text(), f"{digest(binary)}  {stage.BINARY}\n")
            self.assertEqual((root / "plugin-ci-source" / source.name).read_bytes(), source.read_bytes())
            self.assertEqual((root / "plugin-ci-source" / "SHA256SUMS").read_text(), f"{digest(source)}  {source.name}\n")
            self.assertEqual((root / "plugin-ci-source" / "PAYLOAD-SHA256SUMS").read_text(), payload)
            with self.assertRaises(ValueError):
                stage.prepare(root, stage.BINARY)

if __name__ == "__main__":
    unittest.main()
