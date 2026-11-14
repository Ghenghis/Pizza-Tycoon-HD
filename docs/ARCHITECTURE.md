# Engine Architecture

Pizza Legacy is a new C++20 engine written from scratch to reproduce the
original game's behavior while providing a modern, portable way to play it.
It is not a decompilation or port of the original engine; it was developed
through a combination of gameplay observation, asset file analysis, and study
of the disassembled DOS executable.

## Original asset format support

The engine parses the original 1994 asset formats:

- **BOB sprites** with animation sequences
- **VGA graphics** with indexed-color palettes
- **FNT bitmap fonts**
- **CMF music** (Creative Music Format, OPL3 FM synthesis via libADLMIDI)
- **VOC audio** (Creative Voice Format, decoded and resampled to stereo)
- City map data, pizza recipes, restaurant layouts, and character data from
  original formats (English version only)

All assets are loaded on demand through a template-based `ResourceManager<T, Loader>`
that caches resources by filename.

## Type-safe coordinate systems

The engine uses phantom types to make coordinate spaces distinct at compile
time. `WorldPoint`, `ViewPoint`, and `ScreenPoint` are separate types that
cannot be accidentally mixed. Camera and viewport transforms convert between
them explicitly. This is zero-cost: the types have the same runtime
representation as their underlying data (e.g. int). The same pattern is used
for IDs (players, restaurants, cities) to prevent mix-ups.

## Rendering pipeline

Rendering uses a layered composition model with per-layer dirty tracking:

- Each view implements a `Renderable` interface with dirty-state tracking
- Layers are cached to SDL surfaces and only recomposed when marked dirty
- 8-bit indexed-color surfaces are expanded to RGBA during composition
- A CRT scaling mode upscales the native 640x400 to 640x480 to approximate
  the original CRT aspect ratio

Screens are managed as a stack, with an overlay system composited on top.
Popups, tooltips, and a debug layer are all independent
overlays that can block or pass through input to the view beneath them.
Screen transitions use palette-based fade-out/fade-in.

## HD rendering (Pizza Tycoon HD)

Game logic and layout still run entirely in the 640x400 indexed space; HD
only changes what reaches the screen when `hd_scale` is 2 or more. The
pieces, in the order a frame meets them (details and measurements in
`docs/HD_REMASTER.md`):

- **HD pack** (`resources::HdPack`). Loading a VGA picture or a BOB frame
  looks up the matching PNG, resizes it once to exactly the original size x
  2 x `hd_scale` and attaches it to the surface. The pictures a session used
  are listed in `hd_warm.txt` next to the settings; at the next start one
  background thread decodes them while the intro plays (stopping at 1.5 GiB),
  and the renderer streams them to the GPU for up to 3 ms per frame.
- **Draw lists** (`gfx::HdList`). Each `gfx::Surface` keeps, next to its
  indexed pixels, the list of HD pictures drawn on it: picture, source part,
  destination and an optional tint. A blit appends the source's entries,
  clipped and moved; plain indexed pixels drawn over HD end the entries under
  them, so the original shows through. No HD texels are copied on the CPU.
- **GPU compositing** (`HdCompositor`). Per layer, it expands the indexed
  pixels at engine resolution and draws the draw list as textured quads into
  a render target `hd_scale` times larger. Pictures live in GPU textures
  (1.5 GiB budget, least recently used dropped first); small ones (lines of
  text among them) share atlas pages, sent in one upload per page. The
  `Renderer` keeps up to 6 layer render targets in a pool and reuses them
  from screen to screen, then combines the layers and scales the frame to
  the window. Only textured quads and premultiplied-alpha blending are used,
  so every SDL renderer works. `PLEGACY_CPU_COMPOSE=1` selects the CPU
  fallback, which expands each layer on the CPU (rows split across threads)
  and gives the same picture.
- **HD text** (`resources::HdTypeface`, `Font`). At HD resolution each line
  of text is laid out with the original font's line box, alignment and
  baseline and drawn with a TrueType face from `data/fonts` (stb_truetype,
  at the exact texel size). The letters exist only in the draw list: the
  indexed pixels get none. Printable ASCII comes from a glyph sheet per font
  size, built on background threads while the intro plays, as one tinted
  quad per letter (shadow first); other lines, and lines narrowed to fit,
  become one picture each in the atlas (pictures capped at 128 MiB on the
  CPU). The `hd_text` option turns it off.
- **Lighting** (`lighting::Scene`, `LightPass`). A lit screen (the
  restaurant) reports its lights and ambient level each frame; after
  `HdCompositor` has drawn the layer, `LightPass` multiplies it by a light
  map built at engine resolution and adds a halo on the lights, on the GPU
  with fixed-function blending. The `lighting` option turns it off; the CPU
  fallback has none.
- **Smooth motion** (`GuestWalkScene`). Walking guests and waiters are drawn
  between their previous and next cell at the screen's frame rate. Only the
  drawing changes: an HD gate checks that the game state after 2,000 ticks
  is byte-for-byte the same with `smooth_motion` on and off.

## Input system

Input is abstracted into two layers: physical keys (SDL backend) and semantic
actions (game-level). Key bindings support chords (e.g. Ctrl+Shift+B) and are
fully configurable, on the Key bindings screen or in `bindings.conf`. Listeners register for
actions with priority ordering, and an IME-aware text input mode uses RAII
tokens for safe session management. Continuous axes are computed from input
sources to support keyboard and gamepad input.

## Interaction and UI framework

Mouse interaction is based on rectangular hit regions registered with
callbacks, per layer. Events include enter/leave/hover, click, double-click,
scrolling and drag-and-drop. Registration uses RAII tokens for automatic
cleanup. Tooltips, custom cursors, and exclusive capture modes are built in.

## Audio

The audio system mixes sound effects in real time and plays music through OPL3
FM synthesis. VOC samples are decoded from 8-bit mono to 16-bit stereo
(duplicated channels). Separate volume controls for master, music, and effects,
with gain control.

## Debug console and asset viewers

A built-in debug console accepts commands at runtime for inspecting and
manipulating game state. The engine also ships with dedicated viewer modes for
the original asset formats (sprites, graphics, fonts, palettes, sounds),
launchable via CLI flags, used for helping reverse engineering the original
game's data files.

## Cross-platform

Pizza Tycoon HD releases are Windows only: a zip (game, data, DLLs, pack
tools) and the bare exe, made by `scripts/release.ps1` or
`scripts/build-windows-cross.sh`. The Linux and macOS builds are for
development. Upstream Pizza Legacy publishes Windows, Linux and macOS
releases. The engine is designed to be highly portable and should build on
most modern desktop platforms with SDL3 support, including the BSDs and
other Unix-like systems.

Only the primary release platforms are regularly tested, so support for BSD and
other secondary platforms is currently best-effort, but portability is a major
project goal and platform-specific code is kept to a minimum.

Save data and configuration files are stored in the appropriate user data
directories for each platform. Besides those, the program reads:

- the original game's files, from the folder you selected (`--game-path`);
- `data/` next to the program (game data tables, `data/fonts` for HD text)
  and the HD pack (`hd_pack`, `--hd-pack`, or an `hd` folder next to the
  program);
- `hd_warm.txt` next to the settings, which it also writes;
- an `--exec` script, when given one.

It writes only to the user data folder, except where a command is told to:
`--export-assets <dir>` and the console's `screenshot`, `savevga`,
`dumpgameloc`, `dump fingerprint` and `debug bindings save` (see
`SECURITY.md`).

## Repository layout

- `src/`: the engine and game (C++20); `tests/`: its Catch2 tests.
- `tools/hdpack/`: the HD pack tools (Python), with their pytest tests in
  `tools/hdpack/tests/`; `tools/ci/`: the local CI gates and scans.
- `scripts/`: one command per job, for Windows (`.ps1`) and Linux/macOS
  (`.sh`): doctor, build, lint, scan, test, hd-gates, release, ci-local.
  CI runs on the owner's PC (`scripts/ci-local.*`), not on hosted runners.
- `data/`, `assets/`: files the program ships; `docs/`: documentation.

## User interface: faithful by design

The game screens recreate the 1994 layouts, which the remaster promises to
keep (`docs/HD_REMASTER.md`). So game screens get no search, filter or sort
controls and keep the original's density. New tool UIs (outside the
recreated screens) are held to the usual rules: search, filter and sort on
lists, and help text on every control.
