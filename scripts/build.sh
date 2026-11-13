#!/bin/sh
# Release build with tests: build/release/plegacy and build/release/test_plegacy
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="${DEPS_PREFIX:-$ROOT/deps/prefix}"
BUILD="${BUILD_DIR:-$ROOT/build/release}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
[ -d "$PREFIX" ] || "$ROOT/scripts/fetch-deps.sh"
[ -f "$ROOT/third_party/libadlmidi/CMakeLists.txt" ] || "$ROOT/scripts/fetch-deps.sh"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DPLEGACY_BUILD_TESTS=ON -DUSE_CCACHE=OFF >/dev/null
LOG="$BUILD/build.log"
cmake --build "$BUILD" -j "$JOBS" >"$LOG" 2>&1 || { tail -40 "$LOG"; exit 1; }
if grep -E "warning:" "$LOG" >/dev/null; then
  grep -E "warning:" "$LOG"
  echo "FAIL compiler warnings (zero allowed)"; exit 1
fi
echo "ok  build: $BUILD/plegacy (0 warnings)"
