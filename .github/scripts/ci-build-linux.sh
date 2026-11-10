#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${RUNNER_TEMP:?missing RUNNER_TEMP}"
version=32.2.0
sha=c333e4a7d5c4a94c7bb4833f046368e0ac5e981fb3964ca4981b23be1dfbda4a
work="$RUNNER_TEMP/gop-rot-ci"
archive="$work/obs-studio-$version.tar.gz"
sdk="$work/obs-sdk"
build="$work/plugin-build"
stage="$work/package"
out="$work/artifacts"
mkdir -p "$work" "$out"
curl -fsSL --retry 5 "https://github.com/obsproject/obs-studio/archive/refs/tags/$version.tar.gz" -o "$archive"
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$work"
cmake -S "$work/obs-studio-$version" -B "$work/obs-build" -G Ninja \
 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdk" \
 -DOBS_CMAKE_VERSION:STRING=3.0.0 -DOBS_VERSION_OVERRIDE:STRING="$version" \
 -DENABLE_PLUGINS=OFF -DENABLE_FRONTEND=OFF
cmake --build "$work/obs-build" --target obs-frontend-api libobs-opengl --parallel 4
cmake --install "$work/obs-build" --component Development --config Release --prefix "$sdk"
cmake --install "$work/obs-build" --component Runtime --config Release --prefix "$sdk"
test -s "$sdk/lib/cmake/libobs/libobsConfig.cmake"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 -Dlibobs_DIR="$sdk/lib/cmake/libobs"
cmake --build "$build" --parallel 4
listed="$(ctest --test-dir "$build" -N)"
printf '%s\n' "$listed"
printf '%s\n' "$listed" | grep -F 'Total Tests: 10'
printf '%s\n' "$listed" | grep -F 'mosh_obs_scene_projection_camera'
export LD_LIBRARY_PATH="$sdk/lib:$sdk/lib/obs-plugins:${LD_LIBRARY_PATH:-}"
export LIBGL_ALWAYS_SOFTWARE=1
xvfb-run -a -s '-screen 0 1280x1024x24' ctest --test-dir "$build" --output-on-failure
cmake --install "$build" --prefix "$stage/gop-rot"
module="$stage/gop-rot/bin/64bit/gop-rot.so"
test -s "$module"
cp "$root/COPYING" "$root/README.md" "$stage/gop-rot/"
printf '%s\n' "$version" > "$stage/gop-rot/OBS_SDK_VERSION"
if readelf -d "$module" | grep -E '(RPATH|RUNPATH)' | grep -F "$work"; then
 echo '::error::CI-specific RUNPATH found in installable plugin'; exit 1
fi
test "$(find "$stage/gop-rot" -type f | wc -l)" -eq 4
(cd "$stage" && find gop-rot -type f -print0 | sort -z | xargs -0 sha256sum) > "$out/SHA256SUMS"
tar -C "$stage" -czf "$out/gop-rot-linux-x86_64-obs32.tar.gz" gop-rot
tar -tzf "$out/gop-rot-linux-x86_64-obs32.tar.gz" | grep -Fx 'gop-rot/bin/64bit/gop-rot.so'
git -C "$root" archive --format=tar --prefix=gop-rot/ HEAD | gzip -9 > "$out/gop-rot-source.tar.gz"
tar -tzf "$out/gop-rot-source.tar.gz" | grep -Fx 'gop-rot/CMakeLists.txt'
# Never upload internal work diaries, environment files, or agent-private state.
if tar -tzf "$out/gop-rot-source.tar.gz" |
  grep -E '^gop-rot/(ACTIVE_WORK[.]md|[.]env($|[.])|[.]meristem/local/)'; then
  echo '::error::Private local state leaked into source archive'
  exit 1
fi
sha256sum "$out/"*.tar.gz
