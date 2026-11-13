#!/bin/sh
# Local CI (no hosted runners): clean-clone-safe dependencies, build with
# zero warnings, all tests, then a headless HD smoke run if game files exist.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
"$ROOT/scripts/fetch-deps.sh"
"$ROOT/scripts/build.sh"
"$ROOT/scripts/test.sh"
BUILD="${BUILD_DIR:-$ROOT/build/release}"
[ -d "$BUILD" ] && BUILD=$(cd "$BUILD" && pwd)
if [ -n "${PLEGACY_TEST_GAME_PATH:-}" ]; then
  echo "== HD smoke run (export, pack, render at 3x)"
  T="$(mktemp -d)"
  export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
  "$BUILD/plegacy" --config "$T/settings" --game-path "$PLEGACY_TEST_GAME_PATH" --export-assets "$T/export" >/dev/null
  python3 "$ROOT/tools/hdpack/hdpack.py" upscale "$T/export" "$T/pack" --scale 6 --only 'gfx/*' --only 'bob/moebel/*' >/dev/null
  python3 "$ROOT/tools/hdpack/hdpack.py" validate "$T/export" "$T/pack" >/dev/null
  printf 'nav restaurant @\nscreenshot %s/restaurant.png\nnav euromap\nscreenshot %s/euromap.png\n' "$T" "$T" >"$T/smoke.txt"
  "$BUILD/plegacy" --config "$T/settings" --game-path "$PLEGACY_TEST_GAME_PATH" --seed 7 \
    --hd-scale 3 --hd-pack "$T/pack" --exec "$T/smoke.txt" >/dev/null
  python3 - "$T" <<'PY'
import sys
from PIL import Image
for name in ("restaurant", "euromap"):
    im = Image.open(f"{sys.argv[1]}/{name}.png")
    assert im.size == (1920, 1200), (name, im.size)
    assert len(set(im.convert("RGB").resize((64, 40)).getdata())) > 50, name
print("ok  HD smoke run: screens render at 1920x1200")
PY
  rm -rf "$T"
fi
echo "ok  local CI passed"
