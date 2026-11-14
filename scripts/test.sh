#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The checks that need a build but no GPU (the twin of test.ps1):
# clang-format on the fork's C++ files, a smoke run (--version, --help), the
# C++ tests, the pack tool's tests and the CI tools' tests. Python lint is
# scripts/lint.sh.
# Tests that need the original game skip without it, and each skip is
# listed; when the game is expected (PLEGACY_TEST_GAME_PATH or
# PLEGACY_REQUIRE_GAME=1) a skipped test fails the run.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
BUILD="${BUILD_DIR:-$ROOT/build/release}"
[ -d "$BUILD" ] || die "no build at $BUILD" "scripts/build.sh"
BUILD=$(cd "$BUILD" && pwd)
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
GAME_FIX="set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE"

echo "== format (clang-format, files this fork adds or changes)"
# Upstream files keep upstream's formatting; clang-format versions differ in
# small ways, so only our own changes are held to the local version.
# The upstream import: its tag, or the first commit (which is that import).
BASE=upstream-pizzalegacy-1.0.0-beta.2
git -C "$ROOT" rev-parse -q --verify "$BASE" >/dev/null 2>&1 ||
  BASE=$(git -C "$ROOT" rev-list --max-parents=0 HEAD 2>/dev/null | tail -1)
if [ -n "$BASE" ] && git -C "$ROOT" rev-parse -q --verify "$BASE" >/dev/null 2>&1; then
  FILES=$( (git -C "$ROOT" diff --name-only "$BASE" -- src tests
    git -C "$ROOT" ls-files --others --exclude-standard -- src tests) |
    grep -E '\.(cpp|h|inl)$' | sort -u)
  for f in $FILES; do if [ -f "$ROOT/$f" ]; then echo "$ROOT/$f"; fi; done |
    xargs -r clang-format --dry-run --Werror
  ok "clang-format: $(echo "$FILES" | wc -w) files"
else
  skip_check clang-format "not a git checkout, so the fork's files are unknown" "" other
fi

echo "== smoke run (the game starts and exits cleanly; no game files needed)"
[ -x "$BUILD/plegacy" ] || die "no game at $BUILD/plegacy" "scripts/build.sh"
"$BUILD/plegacy" --version >/dev/null || die "plegacy --version failed" "run $BUILD/plegacy --version to see why"
"$BUILD/plegacy" --help >/dev/null 2>&1 || die "plegacy --help failed" "run $BUILD/plegacy --help to see why"
ok "smoke: --version and --help exit with 0"

# skipped_count log: the number in the test runner's last "N skipped".
skipped_count() { sed -n 's/.*[^0-9]\([0-9][0-9]*\) skipped.*/\1/p' "$1" | tail -n 1; }

echo "== C++ tests"
[ -x "$BUILD/test_plegacy" ] || die "no test build at $BUILD/test_plegacy" "scripts/build.sh (it builds the tests too)"
LOG="$BUILD/test_plegacy.log"
if ! "$BUILD/test_plegacy" >"$LOG" 2>&1; then
  cat "$LOG"
  die "C++ tests failed" "see the output above (also in $LOG)"
fi
cat "$LOG"
N=$(skipped_count "$LOG")
if [ -n "$N" ]; then skip_check "C++ tests" "$N test case(s) skipped (they need the original game)" "$GAME_FIX" game; fi
ok "C++ tests"

echo "== pack tool tests (pytest)"
PYTEST=$(pytool pytest)
[ -n "$PYTEST" ] || die "pytest is not installed" "$PYTHON -m pip install -r tools/hdpack/requirements-dev.txt"
EXPORT=""
if [ -n "${PLEGACY_TEST_GAME_PATH:-}" ]; then
  EXPORT="$(mktemp -d)"
  trap 'rm -rf "$EXPORT"' EXIT
  "$BUILD/plegacy" --config "$EXPORT/settings" --game-path "$PLEGACY_TEST_GAME_PATH" --export-assets "$EXPORT/export" >/dev/null
  export PLEGACY_TEST_EXPORT_DIR="$EXPORT/export"
fi
LOG="$BUILD/pytest.log"
# shellcheck disable=SC2086 # "python3 -m pytest" or a path
if ! (cd "$ROOT/tools/hdpack" && $PYTEST -q -rs) >"$LOG" 2>&1; then
  cat "$LOG"
  die "pack tool tests failed" "see the output above (also in $LOG)"
fi
cat "$LOG"
N=$(skipped_count "$LOG")
if [ -n "$N" ]; then skip_check "pack tool tests" "$N test(s) skipped (they need the original game)" "$GAME_FIX" game; fi

echo "== CI tool tests (pytest: hd_gates.py, check_release.py, on synthetic screenshots and releases)"
LOG="$BUILD/pytest-ci.log"
# shellcheck disable=SC2086 # "python3 -m pytest" or a path
if ! (cd "$ROOT/tools/ci" && $PYTEST -q -rs) >"$LOG" 2>&1; then
  cat "$LOG"
  die "CI tool tests failed" "see the output above (also in $LOG)"
fi
cat "$LOG"
N=$(skipped_count "$LOG")
if [ -n "$N" ]; then skip_check "CI tool tests" "$N test(s) skipped" "run them with -rs to see why" other; fi
ok "tests"
