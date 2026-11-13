#!/bin/sh
# Every quality gate that does not need a GPU. Game files are optional:
# set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE to run the tests
# that use the original art (they SKIP otherwise).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${BUILD_DIR:-$ROOT/build/release}"
[ -d "$BUILD" ] && BUILD=$(cd "$BUILD" && pwd)
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy

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
  for f in $FILES; do [ -f "$ROOT/$f" ] && echo "$ROOT/$f"; done |
    xargs -r clang-format --dry-run --Werror
else
  echo "skipped: not a git checkout"
fi

echo "== C++ tests"
"$BUILD/test_plegacy"

echo "== pack tool: format, lint, types, tests"
cd "$ROOT/tools/hdpack"
# As a module when pip-installed (requirements-dev.txt), else from PATH.
tool() { if python3 -c "import $1" 2>/dev/null; then python3 -m "$@"; else "$@"; fi; }
tool black --check -q .
tool ruff check -q .
tool mypy hdpack.py
if [ -n "${PLEGACY_TEST_GAME_PATH:-}" ]; then
  EXPORT="$(mktemp -d)"
  "$BUILD/plegacy" --config "$EXPORT/settings" --game-path "$PLEGACY_TEST_GAME_PATH" --export-assets "$EXPORT/export" >/dev/null
  PLEGACY_TEST_EXPORT_DIR="$EXPORT/export" python3 -m pytest -q
  rm -rf "$EXPORT"
else
  python3 -m pytest -q
fi
echo "ok  all gates passed"
