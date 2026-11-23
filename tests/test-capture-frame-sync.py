#!/usr/bin/env python3
"""Integration contract: clean isolated OBS capture must play source once per output frame."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

WIDTH, HEIGHT, FPS, FRAMES = 64, 64, 30, 18
PIXELS = WIDTH * HEIGHT


def main():
    exe, module, data = (Path(x).resolve() for x in sys.argv[1:])
    if not shutil.which("xvfb-run"):
        raise RuntimeError("xvfb-run unavailable for OBS capture integration")
    with tempfile.TemporaryDirectory(prefix="framerot-frame-sync-") as d:
        root = Path(d)
        src = root / "source.rgba"
        output = root / "clean.bgra"
        frames = []
        for t in range(FRAMES):
            r = (t * 13 + 17) & 255
            g = (t * 7 + 40) & 255
            b = (t * 19 + 90) & 255
            frames.append((bytes((r, g, b, 255)) * PIXELS,
                           bytes((b, g, r, 255)) * PIXELS))
        src.write_bytes(b"".join(x[0] for x in frames))
        for loops in (1, 2):
            proc = subprocess.run(
                ["xvfb-run", "-a", str(exe), str(module), str(data), str(src),
                 str(WIDTH), str(HEIGHT), str(FPS), str(loops),
                 "clean", str(output)],
                capture_output=True, text=True, timeout=15,
            )
            if proc.returncode != 0:
                raise AssertionError(
                    f"capture exited {proc.returncode}:\n{proc.stdout[-3000:]}\n{proc.stderr[-3000:]}"
                )
            result = output.read_bytes()
            expected = b"".join(x[1] for x in frames) * loops
            if result != expected:
                observed = [tuple(result[i * PIXELS * 4:i * PIXELS * 4+4])
                            for i in range(min(len(result) // (PIXELS * 4),
                                               FRAMES * loops))]
                wanted = [tuple(x[1][:4]) for x in frames] * loops
                raise AssertionError(
                    f"OBS clean captured the wrong source frame(s). "
                    f"Expected BGRA {wanted}, observed {observed}"
                )
            print(f"CAPTURE_SYNC_OK frames={FRAMES * loops} "
                  f"pixels={PIXELS} exact_BGRA=true")


if __name__ == "__main__":
    main()
