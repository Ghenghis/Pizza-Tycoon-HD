#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Windows release from Linux or WSL2 with MinGW-w64: one
# "Pizza Legacy.exe" with no DLL dependencies (SDL3, zlib, libADLMIDI and
# the C++ runtime linked in; it still needs the data folder from the zip),
# then the release zip with SHA-256 checksums, checked by the release gate
# (scripts/check-release.sh).
#   sudo apt-get install g++-mingw-w64-x86-64-posix
#
# The version comes from the build (version.txt, written by CMake from
# project(VERSION)). A release needs a heading for it in docs/CHANGELOG.md,
# a clean tree, HEAD tagged v<version> and no earlier files of that version
# in release/ (FORCE=1 overwrites them). SNAPSHOT=1 skips those checks and
# writes release/snapshot/PizzaTycoonHD_v<version>+<commit>_win64.*
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
# shellcheck source=scripts/deps.env
. "$ROOT/scripts/deps.env"
TOOLCHAIN="$ROOT/cmake/mingw-w64-x86_64.cmake"
SRC="$ROOT/deps/src"
export WIN_DEPS_PREFIX="${WIN_DEPS_PREFIX:-$ROOT/deps/win64}"
BUILD="${BUILD_DIR:-$ROOT/build/win64}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
command -v x86_64-w64-mingw32-g++-posix >/dev/null ||
  die "MinGW-w64 missing" "sudo apt-get install g++-mingw-w64-x86-64-posix"
mkdir -p "$SRC" "$WIN_DEPS_PREFIX"

fetch() { [ -d "$SRC/$1/.git" ] || git clone -q --depth 1 --branch "$3" "$2" "$SRC/$1"; }
build() { # name extra-args...
  name=$1; shift
  stamp="$SRC/$name/.win64-$(git -C "$SRC/$name" rev-parse --short HEAD)"
  [ -f "$stamp" ] && { echo "ok  $name (win64, already built)"; return; }
  cmake -S "$SRC/$name" -B "$SRC/$name/build-win64" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_INSTALL_PREFIX="$WIN_DEPS_PREFIX" "$@" >/dev/null
  cmake --build "$SRC/$name/build-win64" -j "$JOBS" >/dev/null
  cmake --install "$SRC/$name/build-win64" >/dev/null
  touch "$stamp"; echo "ok  $name (win64)"
}
fetch SDL https://github.com/libsdl-org/SDL.git "$SDL_TAG"
fetch zlib https://github.com/madler/zlib.git "$ZLIB_TAG"
fetch libADLMIDI https://github.com/Wohlstand/libADLMIDI.git "$ADLMIDI_TAG"
build SDL -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
build zlib
if [ ! -f "$ROOT/third_party/libadlmidi/CMakeLists.txt" ]; then
  rm -rf "$ROOT/third_party/libadlmidi"
  cp -R "$SRC/libADLMIDI" "$ROOT/third_party/libadlmidi"
  rm -rf "$ROOT/third_party/libadlmidi/.git"
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_PREFIX_PATH="$WIN_DEPS_PREFIX" -DUSE_CCACHE=OFF -DPLEGACY_ADLMIDI_STATIC=ON \
  -DZLIB_INCLUDE_DIR="$WIN_DEPS_PREFIX/include" -DZLIB_LIBRARY="$WIN_DEPS_PREFIX/lib/libzlibstatic.a" >/dev/null

# The version, before the long build: CMake writes it at configure time.
[ -f "$BUILD/version.txt" ] ||
  die "no version file at $BUILD/version.txt" "CMakeLists.txt must write it from project(VERSION) at configure time (see the version step in CMakeLists.txt)"
VERSION=$(head -n 1 "$BUILD/version.txt" | tr -d '\r')
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$' || die "bad version '$VERSION' in $BUILD/version.txt"
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ "${SNAPSHOT:-}" = 1 ]; then
  OUT="$ROOT/release/snapshot"; NAME="PizzaTycoonHD_v${VERSION}+${COMMIT}_win64"
else
  grep -Eq "^Pizza Tycoon HD $(echo "$VERSION" | sed 's/[.+]/\\&/g')( |$)" "$ROOT/docs/CHANGELOG.md" ||
    die "no 'Pizza Tycoon HD $VERSION' heading in docs/CHANGELOG.md" "add the release notes for $VERSION first"
  [ -z "$(git -C "$ROOT" status --porcelain)" ] ||
    die "the working tree has uncommitted changes" "commit them first (a release must match a commit), or SNAPSHOT=1"
  git -C "$ROOT" tag --points-at HEAD | grep -qx "v$VERSION" ||
    die "HEAD ($COMMIT) is not tagged v$VERSION" "git tag -a v$VERSION -m 'Pizza Tycoon HD $VERSION', or SNAPSHOT=1"
  OUT="$ROOT/release"; NAME="PizzaTycoonHD_v${VERSION}_win64"
  if [ "${FORCE:-}" != 1 ] && { [ -e "$OUT/$NAME.zip" ] || [ -e "$OUT/$NAME.exe" ]; }; then
    die "release/$NAME.zip or .exe already exists: a release is never overwritten" "bump the version, or FORCE=1 if those files were never shared"
  fi
fi

LOG="$BUILD/build.log"
cmake --build "$BUILD" -j "$JOBS" >"$LOG" 2>&1 || { tail -40 "$LOG"; die "build failed (log: $LOG)"; }
if grep -E "warning:" "$LOG"; then die "compiler warnings (zero allowed; log: $LOG)"; fi
EXE="$BUILD/Pizza Legacy.exe"
ok "$EXE (0 warnings)"

# Imports must be Windows system DLLs only.
DLLS=$(x86_64-w64-mingw32-objdump -p "$EXE" | sed -n 's/^\s*DLL Name: //p' | sort -u)
for d in $DLLS; do
  case "$(echo "$d" | tr '[:upper:]' '[:lower:]')" in
    kernel32.dll|user32.dll|gdi32.dll|winmm.dll|imm32.dll|ole32.dll|oleaut32.dll|version.dll|setupapi.dll|shell32.dll|advapi32.dll|msvcrt.dll|cfgmgr32.dll|hid.dll|dinput8.dll|api-ms-win-*|ucrtbase.dll|ws2_32.dll|uuid.dll|dxgi.dll|dbghelp.dll|bcrypt.dll|winhttp.dll) ;;
    *) die "$EXE needs a non-system DLL: $d" "link it statically (see the cmake call above)";;
  esac
done
ok "no DLL dependencies (imports: $(echo "$DLLS" | tr '\n' ' '))"

STAGE="$OUT/$NAME"
rm -rf "$STAGE"; mkdir -p "$STAGE/tools/hdpack"
cp "$EXE" "$STAGE/"
cp -R "$BUILD/data" "$STAGE/data"
# Original Pizza Tycoon HD art (title screen); used when hd_scale is 2 or more.
cp -R "$ROOT/assets/hd" "$STAGE/hd"
cp "$ROOT/tools/hdpack/hdpack.py" "$ROOT/tools/hdpack/export.py" "$ROOT/tools/hdpack/pixelart.py" \
  "$ROOT/tools/hdpack/depthmaps.py" "$ROOT/tools/hdpack/redraw.py" "$ROOT/tools/hdpack/review_pairs.py" \
  "$ROOT/tools/hdpack/provenance.py" "$ROOT/tools/hdpack/maps.py" \
  "$ROOT/tools/hdpack/furniture_redraw.json" "$ROOT/tools/hdpack/palettes.json" \
  "$ROOT/tools/hdpack/prompts.json" "$ROOT/tools/hdpack/requirements.txt" "$STAGE/tools/hdpack/"
cp -R "$ROOT/tools/hdpack/workflows" "$STAGE/tools/hdpack/"
cp "$ROOT/README.md" "$ROOT/SECURITY.md" "$ROOT/docs/HD_REMASTER.md" "$ROOT/docs/CHANGELOG.md" "$ROOT/docs/CREDITS.md" "$STAGE/"
cp -R "$ROOT/LICENSES" "$STAGE/LICENSES"
cp "$ROOT/LICENSE" "$STAGE/"
# The original game's files are not ours to ship. The only VGA picture in a
# release is the game location screen the engine draws itself.
GAME_FILES=$(find "$STAGE" -type f \( -iname '*.bob' -o -iname '*.fnt' -o -iname '*.e' -o -iname '*.cmf' \
  -o -iname '*.voc' -o -iname '*.pc' -o -iname '*.xmi' -o -iname '*.mid' \
  -o \( -iname '*.vga' ! -name game_location.vga \) \))
if [ -n "$GAME_FILES" ]; then
  printf '%s\n' "$GAME_FILES"
  die "the release stage holds original game files (listed above)" "remove them from the build's data folder"
fi
(cd "$OUT" && rm -f "$NAME.zip" && "$PYTHON" -m zipfile -c "$NAME.zip" "$NAME" >/dev/null)
cp "$EXE" "$OUT/${NAME}.exe"
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" && sha256sum "${NAME}.exe" > "${NAME}.exe.sha256")

# The release gate: unpack what a player gets and check it. A release that
# fails it is removed, so nothing half-checked is left to ship.
if ! "$ROOT/scripts/check-release.sh" "$OUT/$NAME.zip" "$OUT/${NAME}.exe" "$VERSION"; then
  rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256" "$OUT/${NAME}.exe" "$OUT/${NAME}.exe.sha256"
  die "release $VERSION: the release check failed, so its zip and exe were removed (the stage $STAGE is kept)"
fi
ok "release $VERSION ($COMMIT): $OUT/$NAME.zip and $OUT/${NAME}.exe"
