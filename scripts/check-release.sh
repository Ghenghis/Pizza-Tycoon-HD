#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The release gate (tools/ci/check_release.py; the twin of check-release.ps1),
# run by build-windows-cross.sh on every zip it makes, and usable alone:
# checks every SHA-256, unpacks the zip into a temporary folder (always
# removed), checks its files against tools/ci/release-manifest.txt, then
# runs the unpacked game hidden: --version, and a short --exec run when
# game files are available (PLEGACY_TEST_GAME_PATH). Linux cannot start the
# Windows exe, so those two are skipped (listed) there, unless WSL interop
# or PLEGACY_RELEASE_LAUNCHER (e.g. wine) can; WSL runs them for real.
#
#   scripts/check-release.sh <zip> [<bare exe> [<version>]]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
[ $# -ge 1 ] || die "no zip given" "scripts/check-release.sh <zip> [<bare exe> [<version>]]"
ZIP=$1 EXE=${2:-} VERSION=${3:-}
set -- "$ZIP"
if [ -n "$EXE" ]; then set -- "$@" --exe "$EXE"; fi
if [ -n "$VERSION" ]; then set -- "$@" --version-expected "$VERSION"; fi
if [ -n "${PLEGACY_TEST_GAME_PATH:-}" ]; then set -- "$@" --game-path "$PLEGACY_TEST_GAME_PATH"; fi
if game_required; then set -- "$@" --require-game; fi
if [ -n "${PLEGACY_RELEASE_LAUNCHER:-}" ]; then set -- "$@" --launcher "$PLEGACY_RELEASE_LAUNCHER" --require-run; fi
"$PYTHON" "$ROOT/tools/ci/check_release.py" "$@"
