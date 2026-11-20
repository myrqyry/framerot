#!/usr/bin/env python3
"""Capture real framerot OBS output from a short opaque GIF/video on Linux."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FPS = 30
DEFAULTS = ("clean", "instrument", "memory-melt", "motion-smear", "time-ribbons")
MODES = tuple(f"mode-{n}" for n in range(9))
PRESETS = DEFAULTS + MODES


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path, help="new directory for previews and capture logs")
    p.add_argument("--build", type=Path, default=ROOT / "build-framerot")
    p.add_argument("--module", type=Path, default=Path.home() / ".config/obs-studio/plugins/framerot/bin/64bit/framerot.so")
    p.add_argument("--loops", type=int, choices=range(1, 5), default=2)
    p.add_argument("--preset", action="append", choices=PRESETS)
    p.add_argument("--all", action="store_true", help="capture every individual mode and the recipes")
    a = p.parse_args()
    source, output, build, module = (v.expanduser().resolve() for v in (a.input, a.output, a.build, a.module))
    if not source.is_file() or not module.is_file():
        p.error("input file or installed framerot module not found")
    if output.exists():
        p.error("output directory already exists; refusing to overwrite files")
    selected = tuple(dict.fromkeys(PRESETS if a.all else a.preset or DEFAULTS))
    info = json.loads(run("ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height", "-of", "json",
                          source, capture_output=True, text=True).stdout)
    if not info.get("streams"):
        p.error("no video stream in input")
    w, h = (int(info["streams"][0][k]) for k in ("width", "height"))
    if min(w, h) < 16:
        p.error("input is too small")
    scale = min(1, 640 / max(w, h))
    w, h = (int(n * scale) // 2 * 2 for n in (w, h))
    if min(w, h) < 16:
        p.error("input aspect ratio is too extreme")
    size = w * h * 4
    limit = min(256 * 1024**2, 512 * 1024**2 // a.loops) // size
    run("cmake", "--build", build, "--target", "capture-showcase", "--parallel", 2)
    exe = build / "capture-showcase"
    with tempfile.TemporaryDirectory(prefix="framerot-capture-") as temp:
        work = Path(temp)
        raw = work / "input.rgba"
        run("ffmpeg", "-nostdin", "-v", "error", "-i", source,
            "-vf", f"fps={FPS},scale={w}:{h},format=rgb24,format=rgba",
            "-frames:v", limit + 1, "-pix_fmt", "rgba", "-f", "rawvideo", raw)
        count, remainder = divmod(raw.stat().st_size, size)
        if remainder or count < 2 or count > limit:
            p.error("clip is empty, malformed or too long for the bounded capture; trim and retry")
        output.mkdir(parents=True, exist_ok=False)
        (work / "config").mkdir()
        env = dict(os.environ, XDG_CONFIG_HOME=str(work / "config"))
        raw_args = ["-f", "rawvideo", "-pixel_format", "bgra", "-video_size", f"{w}x{h}", "-framerate", FPS]
        for preset in selected:
            rendered = work / (preset + ".bgra")
            log = output / (preset + ".log")
            with log.open("w") as stream:
                run("xvfb-run", "-a", exe, module, ROOT, raw, w, h, FPS, a.loops,
                    preset, rendered, env=env, stdout=stream, stderr=subprocess.STDOUT)
            if rendered.stat().st_size != count * a.loops * size:
                raise RuntimeError(f"Incomplete capture: {preset}")
            run("ffmpeg", "-nostdin", "-v", "error", *raw_args, "-i", rendered,
                "-vf", "fps=15", "-c:v", "libwebp_anim", "-quality", 80,
                "-compression_level", 5, "-loop", 0, "-an", output / (preset + ".webp"))
            # Keep raw sources for the comparison only; other intermediates are disposable.
            if preset not in ("clean", "instrument"):
                rendered.unlink()
            print(f"Captured {preset}: {output / (preset + '.webp')}", flush=True)
        if all(s in selected for s in ("clean", "instrument")):
            # Limit the combined display width without stretching the source.
            panel_w = min(480, w)
            panel_h = max(2, int(h * panel_w / w) // 2 * 2)
            graph = ";".join(
                f"[{i}:v]fps=15,scale={panel_w}:{panel_h},pad={panel_w}:{panel_h+26}:0:26:color=0x131313,"
                f"drawtext=text='{title}':fontcolor=white:fontsize=14:x=8:y=5[{label}]"
                for i, title, label in ((0, "Original footage", "a"), (1, "framerot / Instrument", "b"))
            ) + ";[a][b]hstack=inputs=2"
            inputs = [*raw_args, "-i", work / "clean.bgra", *raw_args, "-i", work / "instrument.bgra"]
            run("ffmpeg", "-nostdin", "-v", "error", *inputs, "-filter_complex", graph,
                "-c:v", "libwebp_anim", "-quality", 78, "-compression_level", 5,
                "-loop", 0, "-an", output / "before-after.webp")
            run("ffmpeg", "-nostdin", "-v", "error", *inputs, "-filter_complex",
                graph + ",fps=12,split[x][y];[x]palettegen=max_colors=192[p];[y][p]paletteuse=dither=bayer",
                "-loop", 0, output / "before-after.gif")
        print("Complete:", output)


if __name__ == "__main__":
    main()
