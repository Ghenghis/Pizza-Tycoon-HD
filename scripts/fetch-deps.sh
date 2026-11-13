#!/bin/sh
# Builds the pinned dependencies into deps/prefix and puts libADLMIDI where
# CMake picks it up (third_party/libadlmidi). Safe to run again.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/deps.env"
SRC="$ROOT/deps/src"
PREFIX="${DEPS_PREFIX:-$ROOT/deps/prefix}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
mkdir -p "$SRC" "$PREFIX"

fetch() { # name url tag
  if [ ! -d "$SRC/$1/.git" ]; then
    git clone -q --depth 1 --branch "$3" "$2" "$SRC/$1"
  fi
}
build() { # name extra-cmake-args...
  name=$1; shift
  if [ -f "$SRC/$name/.installed-$(git -C "$SRC/$name" rev-parse --short HEAD)" ]; then
    echo "ok  $name (already built)"; return
  fi
  cmake -S "$SRC/$name" -B "$SRC/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@" >/dev/null
  cmake --build "$SRC/$name/build" -j "$JOBS" >/dev/null
  cmake --install "$SRC/$name/build" >/dev/null
  touch "$SRC/$name/.installed-$(git -C "$SRC/$name" rev-parse --short HEAD)"
  echo "ok  $name"
}

fetch SDL https://github.com/libsdl-org/SDL.git "$SDL_TAG"
fetch Catch2 https://github.com/catchorg/Catch2.git "$CATCH2_TAG"
fetch libADLMIDI https://github.com/Wohlstand/libADLMIDI.git "$ADLMIDI_TAG"

# Headless-friendly SDL when no X11/Wayland headers exist (CI, containers);
# a desktop build gets the full set.
SDL_ARGS="-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF"
if [ "$(uname)" = Linux ] && ! pkg-config --exists x11 xcursor 2>/dev/null; then
  SDL_ARGS="$SDL_ARGS -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON"
fi
# shellcheck disable=SC2086
build SDL $SDL_ARGS
build Catch2 -DBUILD_TESTING=OFF

if [ ! -f "$ROOT/third_party/libadlmidi/CMakeLists.txt" ]; then
  rm -rf "$ROOT/third_party/libadlmidi"
  cp -R "$SRC/libADLMIDI" "$ROOT/third_party/libadlmidi"
  rm -rf "$ROOT/third_party/libadlmidi/.git"
fi
echo "ok  libADLMIDI -> third_party/libadlmidi"
echo "Dependencies in $PREFIX"
