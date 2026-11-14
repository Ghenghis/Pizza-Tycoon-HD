#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks what Pizza Tycoon HD needs on Linux/macOS and prints the fix for
# anything missing (the twin of doctor.ps1). REQUIRED checks fail the run
# (exit code 1); OPTIONAL ones are reported only, so a clean clone passes
# before anything is built.
#
#   scripts/doctor.sh [--game-path DIR] [--require-game] [--require-tools] [--comfy URL]
#
# --game-path      folder with PT.EXE (default: $PLEGACY_TEST_GAME_PATH);
#                  REQUIRED when given
# --require-game   game files are expected (also PLEGACY_REQUIRE_GAME=1)
# --require-tools  gitleaks, pip-audit and bandit become REQUIRED
#                  (also PLEGACY_REQUIRE_TOOLS=1)
# --comfy URL      check a ComfyUI server (REQUIRED when given)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"

GAME_PATH="${PLEGACY_TEST_GAME_PATH:-}"
COMFY=""
while [ $# -gt 0 ]; do
  case $1 in
  --game-path) GAME_PATH=${2:?--game-path needs a folder}; shift 2 ;;
  --require-game) PLEGACY_REQUIRE_GAME=1; export PLEGACY_REQUIRE_GAME; shift ;;
  --require-tools) PLEGACY_REQUIRE_TOOLS=1; export PLEGACY_REQUIRE_TOOLS; shift ;;
  --comfy) COMFY=${2:?--comfy needs a URL}; shift 2 ;;
  *) die "unknown option $1" "scripts/doctor.sh [--game-path DIR] [--require-game] [--require-tools] [--comfy URL]" ;;
  esac
done

FAILED=""
MISSING=""
# need class name fix command...: runs the command; its first output line
# describes what was found. class is required or optional.
need() {
  _class=$1 _name=$2 _fix=$3
  shift 3
  if _out=$("$@" 2>/dev/null) && [ -n "$_out" ]; then
    ok "$_class  $_name  ($(printf '%s\n' "$_out" | head -n 1))"
    return 0
  fi
  if [ "$_class" = optional ]; then
    printf 'SKIP %s  %s: not found\n' "$_class" "$_name"
    MISSING="$MISSING $_name"
  else
    bad "$_class  $_name: not found"
    FAILED="$FAILED $_name"
  fi
  note "fix: $_fix"
}
tool_class() { if tools_required; then echo required; else echo optional; fi; }

cmake_ok() {
  _v=$(cmake --version | head -n 1)
  _major=$(echo "$_v" | sed -n 's/[^0-9]*\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1/p')
  _minor=$(echo "$_v" | sed -n 's/[^0-9]*\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\2/p')
  if [ "${_major:-0}" -gt 3 ] || { [ "${_major:-0}" -eq 3 ] && [ "${_minor:-0}" -ge 25 ]; }; then echo "$_v"; fi
}
compiler() { for c in c++ g++ clang++; do if command -v "$c" >/dev/null; then "$c" --version | head -n 1; return; fi; done; }
zlib_dev() { if pkg-config --exists zlib; then echo "zlib $(pkg-config --modversion zlib)"; elif [ -f /usr/include/zlib.h ]; then echo /usr/include/zlib.h; fi; }
python_ok() { "$PYTHON" -c 'import sys; v = sys.version_info; print(sys.version.split()[0]) if v >= (3, 10) else None'; }
pillow_numpy() { "$PYTHON" -c "import PIL, numpy; print('Pillow', PIL.__version__, 'numpy', numpy.__version__)"; }
pyversion() { _c=$(pytool "$1" "${2:-$1}"); if [ -n "$_c" ]; then $_c --version; fi; }
deps_built() { if [ -f "${DEPS_PREFIX:-$ROOT/deps/prefix}/include/SDL3/SDL.h" ]; then echo "${DEPS_PREFIX:-$ROOT/deps/prefix}"; fi; }
game_built() { _b="${BUILD_DIR:-$ROOT/build/release}/plegacy"; if [ -x "$_b" ]; then echo "$_b"; fi; }
game_folder() { if [ -n "$GAME_PATH" ] && { [ -f "$GAME_PATH/PT.EXE" ] || [ -f "$GAME_PATH/pt.exe" ]; }; then echo "$GAME_PATH"; fi; }
nothing() { :; }
comfy() { curl -fsS --max-time 5 "$COMFY/system_stats" | head -c 200; }

PIP_DEV="$PYTHON -m pip install -r tools/hdpack/requirements-dev.txt"
echo "== build tools"
need required git "sudo apt-get install git" git --version
need required "cmake >= 3.25" "sudo apt-get install cmake (or pip install cmake)" cmake_ok
need required ninja "sudo apt-get install ninja-build" ninja --version
need required "C++20 compiler" "sudo apt-get install g++" compiler
need required pkg-config "sudo apt-get install pkg-config" pkg-config --version
need required "zlib headers" "sudo apt-get install zlib1g-dev" zlib_dev
need required clang-format "sudo apt-get install clang-format" clang-format --version

echo "== Python and the lint tools"
need required "python 3.10+" "sudo apt-get install python3" python_ok
need required "Pillow and numpy" "$PIP_DEV" pillow_numpy
for t in ruff black mypy pytest; do need required "$t" "$PIP_DEV" pyversion "$t"; done
need required shellcheck "sudo apt-get install shellcheck (or: $PYTHON -m pip install shellcheck-py)" shellcheck --version

echo "== scanners (scripts/scan.sh skips what is missing)"
need "$(tool_class)" gitleaks "https://github.com/gitleaks/gitleaks/releases (one static binary on PATH)" gitleaks version
need "$(tool_class)" pip-audit "$PYTHON -m pip install pip-audit" pyversion pip_audit pip-audit
need "$(tool_class)" bandit "$PYTHON -m pip install bandit" pyversion bandit

echo "== build state (made by the scripts; not needed before the first build)"
need optional "dependencies built" "scripts/fetch-deps.sh (scripts/build.sh runs it)" deps_built
need optional "game built" "scripts/build.sh" game_built

echo "== game files and ComfyUI"
GAME_FIX="point --game-path (or PLEGACY_TEST_GAME_PATH) at the folder that contains PT.EXE"
if [ -n "$GAME_PATH" ] || game_required; then
  need required "original game" "$GAME_FIX" game_folder
else
  need optional "original game (for the HD gates and the tests that use the original art)" "$GAME_FIX" nothing
fi
if [ -n "$COMFY" ]; then need required "ComfyUI at $COMFY" "start ComfyUI, then run this again" comfy; fi

echo
if [ -n "$MISSING" ]; then note "optional, not found:$MISSING"; fi
if [ -n "$FAILED" ]; then die "doctor: required check(s) failed:$FAILED" "install what is listed above, then run scripts/doctor.sh again"; fi
ok "doctor: every required check passed"
