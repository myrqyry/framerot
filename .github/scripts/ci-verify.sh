#!/usr/bin/env bash
# The existing native/GPU/source verification remains authoritative.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
test "${TARGET:?}" = linux-x86_64
test "${MODULE:?}" = framerot
test "${PUBLIC_NAME:?}" = framerot
test "${OBS_VERSION:?}" = 32.2.0
test "${MINIMUM_TESTS:?}" = 18
test "${ARCHIVE:?}" = framerot-linux-x86_64-obs32.2.0-test.tar.gz
python3 -B .github/scripts/test-readme-links.py
python3 -B .github/scripts/check_readme_links.py README.md
python3 -B .github/scripts/test-ctest-report.py
python3 -B .github/scripts/test-stage-ci-artifacts.py
bash .github/scripts/ci-build-linux.sh
python3 -B .github/scripts/stage-ci-artifacts.py "${RUNNER_TEMP:?}" "$ARCHIVE"
