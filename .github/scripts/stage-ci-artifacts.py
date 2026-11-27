"""Adapt already-verified Framerot archives to the shared CI contract."""
import hashlib
from pathlib import Path
import re
import shutil
import sys

BINARY = "framerot-linux-x86_64-obs32.2.0-test.tar.gz"
LEGACY = "framerot-linux-x86_64-obs32.tar.gz"
SOURCE = "framerot-source.tar.gz"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(root, archive):
    if archive != BINARY:
        raise ValueError("Unexpected shared binary archive name")
    old = root / "framerot-ci" / "artifacts"
    files = [old / n for n in (LEGACY, SOURCE, "ARCHIVE-SHA256SUMS", "SHA256SUMS")]
    for path in files:
        if not path.is_file() or path.is_symlink() or not path.stat().st_size:
            raise ValueError(f"Missing verified file: {path.name}")
    hashes = {name: digest(old / name) for name in (LEGACY, SOURCE)}
    expected = {f"{value}  ./{name}" for name, value in hashes.items()}
    receipt = (old / "ARCHIVE-SHA256SUMS").read_text(encoding="utf-8").splitlines()
    if len(receipt) != len(expected) or set(receipt) != expected:
        raise ValueError("Legacy archive checksums do not match")
    payload = (old / "SHA256SUMS").read_text(encoding="utf-8")
    if not payload.splitlines() or not all(re.fullmatch(r"[a-f0-9]{64}  [^\r\n]+", line)
                                           for line in payload.splitlines()):
        raise ValueError("Invalid installed-file checksum manifest")
    binary_dir, source_dir = root / "plugin-ci", root / "plugin-ci-source"
    if any(p.exists() or p.is_symlink() for p in (binary_dir, source_dir)):
        raise ValueError("Shared artifact directory already exists")
    binary_dir.mkdir()
    source_dir.mkdir()
    for folder, original, name in ((binary_dir, LEGACY, BINARY), (source_dir, SOURCE, SOURCE)):
        shutil.copyfile(old / original, folder / name)
        if digest(folder / name) != hashes[original]:
            raise ValueError("Copied archive digest mismatch")
        (folder / "SHA256SUMS").write_text(f"{hashes[original]}  {name}\n", encoding="utf-8")
    (source_dir / "PAYLOAD-SHA256SUMS").write_text(payload, encoding="utf-8")


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), sys.argv[2])
