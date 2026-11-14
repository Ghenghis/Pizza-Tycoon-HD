#!/bin/sh
# Builds the Windows release from Linux or WSL2 with MinGW-w64: one
# self-contained "Pizza Legacy.exe" (SDL3, zlib, libADLMIDI and the C++
# runtime linked in), then the release zip with SHA-256 checksums.
#   sudo apt-get install g++-mingw-w64-x86-64-posix
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/deps.env"
TOOLCHAIN="$ROOT/cmake/mingw-w64-x86_64.cmake"
SRC="$ROOT/deps/src"
export WIN_DEPS_PREFIX="${WIN_DEPS_PREFIX:-$ROOT/deps/win64}"
BUILD="${BUILD_DIR:-$ROOT/build/win64}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
command -v x86_64-w64-mingw32-g++-posix >/dev/null ||
  { echo "FAIL MinGW-w64 missing: sudo apt-get install g++-mingw-w64-x86-64-posix"; exit 1; }
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
LOG="$BUILD/build.log"
cmake --build "$BUILD" -j "$JOBS" >"$LOG" 2>&1 || { tail -40 "$LOG"; exit 1; }
if grep -E "warning:" "$LOG" >/dev/null; then grep -E "warning:" "$LOG"; echo "FAIL compiler warnings"; exit 1; fi
EXE="$BUILD/Pizza Legacy.exe"
echo "ok  $EXE (0 warnings)"

# Imports must be Windows system DLLs only.
DLLS=$(x86_64-w64-mingw32-objdump -p "$EXE" | sed -n 's/^\s*DLL Name: //p' | sort -u)
for d in $DLLS; do
  case "$(echo "$d" | tr 'A-Z' 'a-z')" in
    kernel32.dll|user32.dll|gdi32.dll|winmm.dll|imm32.dll|ole32.dll|oleaut32.dll|version.dll|setupapi.dll|shell32.dll|advapi32.dll|msvcrt.dll|cfgmgr32.dll|hid.dll|dinput8.dll|api-ms-win-*|ucrtbase.dll|ws2_32.dll|uuid.dll|dxgi.dll|dbghelp.dll|bcrypt.dll|winhttp.dll) ;;
    *) echo "FAIL $EXE needs a non-system DLL: $d"; exit 1;;
  esac
done
echo "ok  self-contained (imports: $(echo $DLLS | tr '\n' ' '))"

VERSION=$(sed -n 's/^Pizza Tycoon HD \([^ ]*\).*/\1/p' "$ROOT/docs/CHANGELOG.md" | head -1)
OUT="$ROOT/release"; NAME="PizzaTycoonHD_v${VERSION}_win64"; STAGE="$OUT/$NAME"
rm -rf "$STAGE"; mkdir -p "$STAGE/tools/hdpack"
cp "$EXE" "$STAGE/"
cp -R "$BUILD/data" "$STAGE/data"
# Original Pizza Tycoon HD art (title screen); used when hd_scale is 2 or more.
cp -R "$ROOT/assets/hd" "$STAGE/hd"
cp "$ROOT/tools/hdpack/hdpack.py" "$ROOT/tools/hdpack/export.py" "$ROOT/tools/hdpack/pixelart.py" \
  "$ROOT/tools/hdpack/depthmaps.py" "$ROOT/tools/hdpack/palettes.json" \
  "$ROOT/tools/hdpack/prompts.json" "$ROOT/tools/hdpack/requirements.txt" "$STAGE/tools/hdpack/"
cp -R "$ROOT/tools/hdpack/workflows" "$STAGE/tools/hdpack/"
cp "$ROOT/README.md" "$ROOT/SECURITY.md" "$ROOT/docs/HD_REMASTER.md" "$ROOT/docs/CHANGELOG.md" "$ROOT/docs/CREDITS.md" "$STAGE/"
cp -R "$ROOT/LICENSES" "$STAGE/LICENSES"
(cd "$OUT" && rm -f "$NAME.zip" && python3 -m zipfile -c "$NAME.zip" "$NAME" >/dev/null)
cp "$EXE" "$OUT/${NAME}.exe"
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" && sha256sum "${NAME}.exe" > "${NAME}.exe.sha256")
echo "ok  release: $OUT/$NAME.zip and $OUT/${NAME}.exe"
