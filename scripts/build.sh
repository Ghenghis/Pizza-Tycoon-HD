#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Release build with tests: build/release/plegacy and build/release/test_plegacy.
# Fails on any compiler warning and on any CMake warning from this project's
# own CMake files.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
PREFIX="${DEPS_PREFIX:-$ROOT/deps/prefix}"
BUILD="${BUILD_DIR:-$ROOT/build/release}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
[ -d "$PREFIX" ] || "$ROOT/scripts/fetch-deps.sh"
[ -f "$ROOT/third_party/libadlmidi/CMakeLists.txt" ] || "$ROOT/scripts/fetch-deps.sh"
mkdir -p "$BUILD"
CONFIGURE_LOG="$BUILD/configure.log"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DPLEGACY_BUILD_TESTS=ON -DUSE_CCACHE=OFF -Wno-dev >"$CONFIGURE_LOG" 2>&1 ||
  { tail -40 "$CONFIGURE_LOG"; die "CMake configure failed (log: $CONFIGURE_LOG)"; }
# Third-party subprojects are not ours to fix; this project's own warnings are.
if grep -E '^CMake Warning( \(dev\))? at (CMakeLists\.txt|cmake/)' "$CONFIGURE_LOG"; then
  die "CMake warnings (zero allowed; log: $CONFIGURE_LOG)"
fi
LOG="$BUILD/build.log"
cmake --build "$BUILD" -j "$JOBS" >"$LOG" 2>&1 || { tail -40 "$LOG"; die "build failed (log: $LOG)"; }
# Only files compiled in this run print their warnings.
if grep -E "warning:" "$LOG"; then
  die "compiler warnings (zero allowed; log: $LOG)"
fi
ok "build: $BUILD/plegacy (0 warnings)"
