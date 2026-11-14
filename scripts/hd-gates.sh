#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end HD gates (tools/ci/hd_gates.py; the twin of hd-gates.ps1),
# hidden: the HD screens render, GPU and CPU compositing draw the same
# picture, smooth motion leaves the game state unchanged, the 4K frame time
# is measured, and no major screen loses HD coverage (against
# tools/ci/coverage-budget.json; report in build/coverage). Screenshots and
# game logs of a failed run are kept under build/hd-gates.
#
# Without game files the gates are skipped (listed); with
# PLEGACY_REQUIRE_GAME=1 that is a failure. With game files every gate must
# run. PLEGACY_TEST_HD_PACK picks a pack; without one a baseline pack is
# built from the game's art. SDL's software renderer is used (headless
# machines, where frame times are reported only); PLEGACY_HD_GATES_GPU=1
# uses the real GPU and enforces the frame-time limits.
# PLEGACY_COVERAGE_UPDATE=1 runs only the coverage gate and records today's
# shares as the budget for this pack and renderer (review build/coverage,
# then commit tools/ci/coverage-budget.json).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
if [ -z "${PLEGACY_TEST_GAME_PATH:-}" ]; then
  skip_check "HD gates" "no game folder" "set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE" game
  exit 0
fi
[ -f "$PLEGACY_TEST_GAME_PATH/PT.EXE" ] || [ -f "$PLEGACY_TEST_GAME_PATH/pt.exe" ] ||
  die "no PT.EXE in $PLEGACY_TEST_GAME_PATH" "point PLEGACY_TEST_GAME_PATH at the folder that contains PT.EXE"
BUILD="${BUILD_DIR:-$ROOT/build/release}"
set -- --game "$BUILD/plegacy" --game-path "$PLEGACY_TEST_GAME_PATH" --fail-on-skip --artifacts "$ROOT/build/hd-gates"
if [ -n "${PLEGACY_TEST_HD_PACK:-}" ]; then set -- "$@" --pack "$PLEGACY_TEST_HD_PACK"; else set -- "$@" --make-pack; fi
if [ "${PLEGACY_HD_GATES_GPU:-}" != 1 ]; then set -- "$@" --software --bench-frames 20; fi
if [ "${PLEGACY_COVERAGE_UPDATE:-}" = 1 ]; then set -- "$@" --checks coverage --coverage-update; fi
"$PYTHON" "$ROOT/tools/ci/hd_gates.py" "$@"
ok "HD gates"
