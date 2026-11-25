# Building and validating framerot

[Back to the README](../README.md) · [User guide](guide.md) · [Technical reference](technical.md)

## Linux build and isolated validation

Requires CMake, Ninja, the OBS/libobs SDK, FFmpeg development libraries (libavcodec, libavutil, libswscale), pkg-config, a C/C++ compiler, and xvfb-run/Mesa for headless OBS tests. Build against an OBS SDK **no newer than the installed OBS runtime**.

Run from this checkout:

~~~bash
cmake -S . -B build-framerot -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -Dlibobs_DIR="/path/to/OBS-32/lib/cmake/libobs"
cmake --build build-framerot --parallel 4

# Runs the genuine FFmpeg packet test and the real OBS/OpenGL filter test
xvfb-run -a ctest --test-dir build-framerot --output-on-failure

# Stage a copy away from your installed OBS, without activating it:
cmake --install build-framerot --prefix /tmp/framerot-stage
~~~

Replace the placeholder path with a directory containing your compatible
libobsConfig.cmake. OBS SDK locations vary among distributions and source builds.
The runtime OBS version must not be older than the build SDK.

The native output is build-framerot/framerot.so. Tests load the module into an
isolated headless OBS instance without changing daily scenes or plugins.

To install *after* testing and deciding to use it:

~~~bash
cmake --install build-framerot \
  --prefix "$HOME/.config/obs-studio/plugins/framerot"
~~~

Restart OBS after installation. Do **not** install to system OBS directories.
For a prebuilt Linux CI package, extract the archive into
~/.config/obs-studio/plugins/ (it already contains a framerot directory).
The package does not bundle FFmpeg runtime libraries; inspect its included
README and COPYING before installing.

### Optional performance diagnostic

Run on a real Linux desktop GPU (not Xvfb) to compare an unfiltered 1920×1080, 60 FPS synthetic source against Instrument with 288px internal height, 30 codec FPS and active compressed rewriting:

~~~bash
build-framerot/test-mosh-performance "$PWD/build-framerot/framerot.so" "$PWD"

# Optional: length of each measurement (2–60 seconds) and codec height
# (64–576 pixels, multiple of 16). Both intervals use the same settings.
build-framerot/test-mosh-performance "$PWD/build-framerot/framerot.so" "$PWD" 15 576
~~~

Reports OBS frame counts, missed render frames, average render CPU time and filter timing. The synthetic 1080p60 source is measured unfiltered and then with Instrument; the codec still runs at 30 FPS. Example from the local RTX 2070 SUPER on November 22, 2026: at 512×288, 8 seconds per interval, 480/480 baseline and Instrument frames with no lagged frames, and 270/270 codec jobs completed at 1.132ms average. At 1024×576 with 15 seconds per interval, Instrument rendered 900/900 frames without lag and completed 480/480 codec jobs at 5.069ms average / 14.928ms maximum, zero drops/errors. The separate 576px baseline interval did experience 33 lagged frames; these aren't controlled performance comparisons across identical system load. This is not a streaming/encoder load test, GPU stall guarantee, or proof for complex scenes.

## CI builds

The GitHub Actions workflow **Build and test** is configured for pushes and PRs
to `main`, and can also be started manually in the Actions tab. It calls the shared [OBS plugin CI workflow](https://github.com/myrqyry/obs-plugin-ci) with the qualified Linux-only profile. It builds
**OBS 32.2.0** from a SHA-256-verified upstream source archive on a native
Ubuntu 24.04 x86_64 runner, then builds `framerot` against that SDK (not against
the runner's potentially older `libobs-dev` package).

The job runs **all 18 native and capture tests** using FFmpeg and software-rendered OBS/OpenGL
under Xvfb, including landscape/portrait effects and the original scene
geometry regression. Only after those pass does it stage the plugin and upload:

- `framerot-linux-x86_64-obs32.2.0-test.tar.gz`: extract into the OBS plugin directory;
  includes `framerot/bin/64bit/framerot.so`, COPYING, README, the three documentation files and OBS SDK version
- `framerot-source.tar.gz`: tracked source files at the tested Git commit (private local notes and build outputs excluded)
- `SHA256SUMS`: single-archive checksum, in each downloadable artifact
- `PAYLOAD-SHA256SUMS`: checksums for installed-package files, in the separate source artifact

The native test artifact is **Linux x86_64 only** and expects compatible system FFmpeg
libraries (as used by Ubuntu 24.04 and Linux Mint 22). It is not a universal
binary and is not the equivalent of the Windows/macOS distributions available
for some of the other OBS plugins. Those targets need separate, native OBS
SDK and FFmpeg dependency handling before claiming build support. No Docker,
deployments, or automatic updates to your local OBS installation are involved.


## Native portability and codec-only verification

The engine requires only libavcodec, libavutil and libswscale, and uses
portable C++17 synchronization around the C codec/packet editor. The
nine engine tests run without OBS:

~~~bash
cmake -S . -B build-codec-native -DFRAMEROT_CODEC_ONLY=ON \
  -DRETRO_FFMPEG_ROOT=/path/to/compatible/ffmpeg-sdk
cmake --build build-codec-native --config Release
ctest --test-dir build-codec-native -C Release --output-on-failure
~~~

The explicit SDK root must have headers and libraries in one prefix.
Linux can omit the root and use pkg-config. On Windows and macOS provide
a matching SDK and native runtime libraries, not an unrelated FFmpeg ABI.
The Native codec qualification workflow uses the verified OBS 32.2.0
dependency archive pins for Windows x64, Apple Silicon and Intel macOS.
It runs the codec-only suite and does not publish OBS plugins.

For an experimental native module build, omit FRAMEROT_CODEC_ONLY and
supply a matching libobs_DIR plus RETRO_FFMPEG_ROOT. CMake stages a
Windows DLL in bin/64bit or a macOS plugin bundle. Native OBS module
loading, graphics backend testing and packaging are not yet qualified.
Do not distribute the staged module until these checks pass.

The earlier 0.8.0 codec slice was qualified against both FFmpeg 6
and an isolated FFmpeg 8.0 build. The current Spectral Melt / Phantom Drift
features and packet payload rewrite have been verified on FFmpeg 6,
but not yet rerun against FFmpeg 8 or native Windows/macOS builds.


## Capture real effect previews

On Linux, with the normal build configured, the opt-in capture tool uses
an isolated libobs context under Xvfb. It loads the installed module by
default, or a staged/build module with `--module`. It does not touch
the normal OBS scene collection.
Requires FFmpeg with libwebp animation and drawtext support and Python 3.

~~~bash
python3 tools/capture-showcase.py input.gif output-previews
~~~

The input is treated as opaque, resampled to 30 FPS, scaled to at most
640 pixels on its longest side, and looped twice by default. Each preview
uses a fresh filter, one second of stationary warmup, then the original
footage. The default batch writes a clean reference, default Instrument
and three original creative recipes, plus a side-by-side WebP and GIF.
The additional codec recipes are selectable explicitly:

~~~bash
python3 tools/capture-showcase.py input.gif output-new \
  --module ./build-framerot/framerot.so \
  --preset clean --preset phantom-drift --preset spectral-melt
~~~

To compare the opposite frequency treatment, use `--preset clean
--preset spectral-melt --preset spectral-fracture` with a new output directory.
Use `--all` for all individual modes and recipes, or
`--preset mode-0` (repeatable) for specific modes.
`--loops 1` captures one pass. Output must be a new directory. Raw frames
are temporary and bounded, and source files are never modified.
When a capture includes a clean reference and at least one filter, the
wrapper generates a side-by-side comparison. A clean + Instrument capture
preserves the original `before-after.webp` and `before-after.gif` outputs.
Captures with other selections also generate `comparison.webp` and
`comparison.gif` with up to three panels (clean and the first two effects).
Panels are scaled without stretching; this is built from the original raw
30 FPS OBS outputs, rather than comparing encoder-coalesced animation frames.
Animated WebP may collapse identical consecutive frames. Compare captured
effects by playback timestamp, not by stored WebP frame index.

The generated logs record module behavior; these are live OBS renders,
not offline simulations. The preview tool is not a performance benchmark.

## Packet replacement microbenchmark

The opt-in benchmark compares the pre-optimization FFmpeg packet replacement
sequence against the production `framerot_packet_overwrite` implementation:

~~~bash
cmake --build build-framerot --target bench-packet-overwrite
./build-framerot/bench-packet-overwrite
~~~

It times 512 packet rewrites per sample across 23 alternating-order samples
(three warmups), reporting median nanoseconds per operation. Payload sizes
(350, 4,336, 4,638 bytes) are representative of actual MPEG-4 packets from
the 336×192, 18 FPS, 360 kb/s demo clip. The four scenarios are same-size,
shrinking, growing, and shared-buffer rewrites. The benchmark verifies bytes,
timestamps, flags, timebase, side-data counts and preserved shared references.
It intentionally excludes packet input allocation, codec processing,
frame rendering, and streaming overhead. Results depend on CPU/cache,
FFmpeg version, compiler and system load; ratios are not OBS FPS claims.
