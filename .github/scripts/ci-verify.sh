#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
[[ "$MODULE" == framerot && "$PUBLIC_NAME" == framerot && "$MINIMUM_TESTS" == 18 ]]
pin="$(python - "$root/.github/workflows/ci.yml" <<'PYTHON'
import re,sys
from pathlib import Path
matches=re.findall(r'^\s+uses: myrqyry/obs-plugin-ci/\.github/workflows/build\.yml@([0-9a-f]{40})\s*$',Path(sys.argv[1]).read_text(),re.M)
if len(matches)!=1:raise SystemExit("Expected exactly one immutable CI engine pin")
print(matches[0])
PYTHON
)"
engine="$RUNNER_TEMP/obs-ci-engine"
git init -q "$engine"
git -C "$engine" fetch --depth=1 https://github.com/myrqyry/obs-plugin-ci.git "$pin"
git -C "$engine" checkout --detach FETCH_HEAD
[[ "$(git -C "$engine" rev-parse HEAD)" == "$pin" ]]
rustup toolchain install 1.93.1 --profile minimal
cargo +1.93.1 build --release --locked --manifest-path "$engine/Cargo.toml"
export OBS_CI_SCRIPTS="$engine/scripts"
cd "$root"
python -B .github/scripts/test-native-caller.py
python -B .github/scripts/test-ctest-report.py
python -B .github/scripts/test-readme-links.py
python -B .github/scripts/check_readme_links.py README.md
if [[ "$TARGET" != linux-x86_64 ]]; then
    exec python "$engine/scripts/native_caller.py" "$root"
fi
python -B .github/scripts/test-stage-ci-artifacts.py
bash "$engine/scripts/run-linux-caller.sh" "$root"
if [[ "$OBS_VERSION" == 32.2.0 ]]; then
    source="$RUNNER_TEMP/plugin-ci-source"
    mkdir "$source"
    git archive --format=tar.gz --prefix=framerot/ -o "$source/framerot-source.tar.gz" HEAD -- . ':!.meristem' ':!AGENTS.md' ':!ACTIVE_WORK.md'
    (cd "$source" && sha256sum framerot-source.tar.gz > SHA256SUMS)
    obsci="$engine/target/release/obs-ci"
    "$obsci" payload-manifest "$RUNNER_TEMP/plugin-ci/$ARCHIVE" > "$source/PAYLOAD-SHA256SUMS"
    "$obsci" audit-source "$source/framerot-source.tar.gz" --binary "$RUNNER_TEMP/plugin-ci/$ARCHIVE"
fi
