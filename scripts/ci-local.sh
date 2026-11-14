#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Local CI (no hosted runners), the same steps as scripts\ci-local.ps1:
# doctor, dependencies, build with zero warnings, lint, scans, tests and
# smoke run, HD gates. Stops at the first failure. Ends with the list of
# checks that were skipped. Windows packages come from scripts\release.ps1
# or scripts/build-windows-cross.sh, not from this script.
#
#   scripts/ci-local.sh [--require-game] [--require-tools]
#
# --require-game   the original game is expected (also when
#                  PLEGACY_TEST_GAME_PATH is set): skipped game checks fail
# --require-tools  gitleaks, pip-audit and bandit must be installed
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
for a in "$@"; do
  case $a in
  --require-game) PLEGACY_REQUIRE_GAME=1; export PLEGACY_REQUIRE_GAME ;;
  --require-tools) PLEGACY_REQUIRE_TOOLS=1; export PLEGACY_REQUIRE_TOOLS ;;
  *) die "unknown option $a" "scripts/ci-local.sh [--require-game] [--require-tools]" ;;
  esac
done

mkdir -p "$ROOT/build"
PLEGACY_SKIP_LOG="$ROOT/build/ci-local-skipped.txt"
export PLEGACY_SKIP_LOG
: >"$PLEGACY_SKIP_LOG"
summary() {
  if [ -s "$PLEGACY_SKIP_LOG" ]; then
    echo "Skipped ($(grep -c . "$PLEGACY_SKIP_LOG")):"
    sed 's/^/     /' "$PLEGACY_SKIP_LOG"
  else
    note "skipped: nothing"
  fi
}
trap summary EXIT

for step in doctor fetch-deps build lint scan test hd-gates; do
  echo "== $step"
  "$ROOT/scripts/$step.sh"
done
skip_check "package" "Windows packages come from scripts\\release.ps1 or scripts/build-windows-cross.sh" "" other
trap - EXIT
summary
ok "local CI passed"
