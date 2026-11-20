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
~~~

Reports OBS frame counts, missed render frames, average render CPU time and filter timing. This is not a streaming/encoder load test or a guarantee for complex scenes.

## CI builds

The GitHub Actions workflow **Build and test** is configured for pushes and PRs
to `main`, and can also be started manually in the Actions tab. It builds
**OBS 32.2.0** from a SHA-256-verified upstream source archive on a native
Ubuntu 24.04 x86_64 runner, then builds `framerot` against that SDK (not against
the runner's potentially older `libobs-dev` package).

The job runs **all 14 native tests** using FFmpeg and software-rendered OBS/OpenGL
under Xvfb, including landscape/portrait effects and the original scene
geometry regression. Only after those pass does it stage the plugin and upload:

- `framerot-linux-x86_64-obs32.tar.gz`: extract into the OBS plugin directory;
  includes `framerot/bin/64bit/framerot.so`, COPYING, README, the three documentation files and OBS SDK version
- `framerot-source.tar.gz`: tracked source files at the tested Git commit (private local notes and build outputs excluded)
- `SHA256SUMS`: checksums for installed-package files

This artifact is **Linux x86_64 only** and expects compatible system FFmpeg
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

Local Linux qualification: all nine codec tests pass against FFmpeg 6
and an isolated FFmpeg 8.0 build; all 14 Linux tests pass. This is not
evidence of native Windows or macOS execution.


## Capture real effect previews

On Linux, with the normal build configured and framerot installed, the
opt-in capture tool uses an isolated libobs context under Xvfb and the
actual installed module; it does not touch the normal OBS scene collection.
Requires FFmpeg with libwebp animation and drawtext support and Python 3.

~~~bash
python3 tools/capture-showcase.py input.gif output-previews
~~~

The input is treated as opaque, resampled to 30 FPS, scaled to at most
640 pixels on its longest side, and looped twice by default. Each preview
uses a fresh filter, one second of stationary warmup, then the original
footage. The default batch writes a clean reference, default Instrument
and the three editable creative recipes, plus a side-by-side WebP and GIF.
Use `--all` for all individual modes, `--preset mode-0` (repeatable) for
specific modes, or `--module /path/to/framerot.so` to test a staged binary.
`--loops 1` captures one pass. Output must be a new directory. Raw frames
are temporary and bounded, and source files are never modified.

The generated logs record module behavior; these are live OBS renders,
not offline simulations. The preview tool is not a performance benchmark.
