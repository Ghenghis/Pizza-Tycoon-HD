# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared helpers for the sh scripts (the twin of common.ps1). Source it after
# setting ROOT:  . "$ROOT/scripts/common.sh"
# shellcheck shell=sh

PYTHON="${PYTHON:-python3}"

ok() { printf 'PASS %s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; }
note() { printf '     %s\n' "$*"; }

# die message [fix]: prints a FAIL line and the fix, then stops.
die() {
  bad "$1"
  if [ -n "${2:-}" ]; then note "fix: $2"; fi
  exit 1
}

# The original game's files are expected when PLEGACY_REQUIRE_GAME=1 or
# PLEGACY_TEST_GAME_PATH is set: a check that needs them then fails instead
# of skipping. PLEGACY_REQUIRE_TOOLS=1 does the same for optional tools.
game_required() { [ "${PLEGACY_REQUIRE_GAME:-}" = 1 ] || [ -n "${PLEGACY_TEST_GAME_PATH:-}" ]; }
tools_required() { [ "${PLEGACY_REQUIRE_TOOLS:-}" = 1 ]; }

# skip_check name reason fix kind: reports a check that cannot run. kind is
# game, tool or other; a game or tool skip fails when that is required.
# Skips are listed in $PLEGACY_SKIP_LOG, which ci-local summarises.
skip_check() {
  if { [ "$4" = game ] && game_required; } || { [ "$4" = tool ] && tools_required; }; then
    die "$1: $2, and it is required here (PLEGACY_TEST_GAME_PATH, PLEGACY_REQUIRE_GAME or PLEGACY_REQUIRE_TOOLS)" "$3"
  fi
  printf 'SKIP %s: %s\n' "$1" "$2"
  if [ -n "$3" ]; then note "fix: $3"; fi
  if [ -n "${PLEGACY_SKIP_LOG:-}" ]; then printf '%s: %s\n' "$1" "$2" >>"$PLEGACY_SKIP_LOG"; fi
  return 0
}

# pytool module [exe]: prints the command that runs a Python tool (a module
# of $PYTHON when installed there, else the program on PATH), or nothing.
pytool() {
  if "$PYTHON" -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('$1') else 1)" 2>/dev/null; then
    echo "$PYTHON -m $1"
  elif command -v "${2:-$1}" >/dev/null 2>&1; then
    command -v "${2:-$1}"
  fi
}

# run_pytool module args...: runs a required Python tool, or stops with the
# command that installs it.
run_pytool() {
  _m=$1
  shift
  _cmd=$(pytool "$_m")
  [ -n "$_cmd" ] || die "$_m is not installed" "$PYTHON -m pip install -r tools/hdpack/requirements-dev.txt"
  # shellcheck disable=SC2086 # "python3 -m tool" or a path without spaces
  $_cmd "$@"
}
