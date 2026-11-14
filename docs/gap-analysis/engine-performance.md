# Engine performance and runtime gap analysis

Pizza Tycoon HD (Pizza Legacy engine, C++20, SDL 3.4.0), audited 2026-11-14.

**Method.** Static reading only. Nothing was built, run or profiled. The source
is HEAD `8b92b4f` plus the uncommitted work in progress in `src/engine/HdCompositor.*`,
`Renderer.*`, `Game.cpp`, `gfx/HdImage.h`, `gfx/HdList.*`, `gfx/Surface.*`,
`resources/Font.cpp` and `resources/HdTypeface.*`. Line numbers refer to that
working tree, and files that are still being edited may shift. The vendored SDL
is at `deps/src/SDL` (tag `release-3.4.0`, `scripts/deps.env`).

**Measurements used** (Windows, Ryzen 7 5800X3D, RTX 3090 Ti / 3060, D3D11,
3840x2400, `bench`):

* steady fully redrawn frame: about 3 ms on average, 4-5 ms at p99;
* first frame of a screen with HD text on: 19-26 ms, with it off: 12-14 ms;
* split: views 8.5 ms, compositing 8-15 ms (of which text upload 4-10 ms),
  present 1 ms;
* latest text numbers (before glyph sheets): 23 lines, **3.4 MB of text
  pictures but 5.7 MB sent**.

Anything else marked *est.* is a figure I worked out from the code, not a
measurement.

**Classic mode.** Every fix below changes only the picture, the frame timing or
the tooling. None touches `GameState`, the 30 Hz tick, the RNG or the indexed
640x400 surfaces that the game reads back. Where a fix comes near those, the
item says so.

Effort: **S** = up to 1 day, **M** = 2-5 days, **L** = more than a week.

---

## 0. What changed while this audit was being written

* **Glyph sheets for HD text.** `HdTypeface::Sheet` (one white-plus-alpha
  picture per cap height, built by the prewarm thread, `HdTypeface.cpp`
  L129-182) is drawn as one tinted `HdCmd` per letter (`HdList.h` L23-25,
  L117-122), through `Surface::hd_overlay` (`Surface.cpp` L125-147). Lines use
  it only when all letters are ASCII and the line is not narrowed
  (`HdTypeface.cpp` L323, L349). Sheets are uploaded by `stream_ahead`
  (`Renderer.cpp` L156-161).
* Fallback line pictures share atlas pages, flushed once per page
  (`flush_atlas` L275-292). Layer targets are pooled (`take_target` /
  `give_back` L67-99, 6 kept). The bench prints the time breakdown
  (`Game.cpp` L990-1037).

The 5.7 MB sent for 3.4 MB of pictures (68% more than needed) confirms P2. The
items below are written against the new code.

---

## 1. Where the first-frame time goes (model)

| Part (measured) | What the code does there (est.) |
|---|---|
| views 8.5 ms | `View::render` per layer. Sprites are decoded again on every call (P7). HD pack lookups build `std::filesystem::path` keys (P7). HD text letters and pictures. A fresh 1 MB `HdList` floor per layer (P9). Pack files missing from the warm list are decoded on the main thread (L1). |
| compositing 8-15 ms | Per layer, `upload_base` expands the pixels to RGBA and re-creates the shared base texture for every new layer size (P5). A per-pixel run scan (P10). Text and atlas uploads, each one also converted by SDL because the texture format is not native (P1), plus a D3D11 staging texture per call. |
| present 1 ms | `SDL_RenderPresent`. With vsync off, a frame can be dropped silently (P0). |

Removing the conversion (P1), the base-texture churn (P5) and the text uploads
(glyph sheets, P3) should bring a cold screen open to about 10-12 ms (est.),
within the 16 ms target, without moving off `SDL_Renderer`.

---

## Area 1: first frame / screen open

### P0. The benchmark hides real costs (do this first)
- **Problem.** `bench` measures an easier case than a real screen open. It
  builds a fresh `Renderer` with the view's layers only, without the 8
  overlays (`Game.cpp` L957-958; `Overlays` L97-108). It pre-creates targets
  and uploads everything (`warm_up_gpu`, L963). It turns vsync off (L964),
  and with vsync off SDL's D3D11 backend presents with
  `DXGI_PRESENT_DO_NOT_WAIT` (`SDL_render_d3d11.c` L987-999) and treats
  `DXGI_ERROR_WAS_STILL_DRAWING` as success (L2872). A GPU-bound frame is then
  **not presented**, and its GPU time never reaches the average.
- **Fix.** Add `bench cold`: overlays included, no warm-up, the same
  `clear_layers` / `add_layers` path as `Game::run`. End each run with a 1x1
  `SDL_RenderReadPixels` (a GPU sync) and report wall time / frames next to
  CPU time, flagged "GPU-bound" when they differ by more than 10%. Optionally
  add D3D11 timestamp queries via `SDL_PROP_RENDERER_D3D11_DEVICE_POINTER`.
  Add the cold number to `hd_gates.py --max-first`.
- **Gain.** Numbers that match what players see, especially on the 3060 and
  iGPUs. **Risk:** none. **Effort:** S.

### P1. Every texture is created in a format no GPU backend supports, so SDL converts on every upload
- **Problem.** Texels are `0xRRGGBBAA` (`SDL_PIXELFORMAT_RGBA8888`,
  `gfx/HdImage.h` L20-21, L74-85), and every texture is created that way
  (`Renderer.cpp` L77, L130; `HdCompositor.cpp` L103, L194, L461;
  `LightPass.cpp` L55, L73; `Game.cpp` L1194, L1203).
  - D3D11 and D3D12 offer only ARGB8888 / ABGR8888 / XRGB8888
    (`SDL_render_d3d11.c` L2965-2969, `SDL_render_d3d12.c` L3535-3539);
    Vulkan, OpenGL and `gpu` offer BGRA32 / RGBA32. So SDL wraps each texture
    around a *native* one (`SDL_render.c` L1565-1601, L1433-1480).
  - Each `SDL_UpdateTexture` on a static texture then does `SDL_malloc` of
    the rect, a full swizzle and `SDL_free` (`SDL_UpdateTextureNative`
    L2281-2314), before D3D11 makes its own staging texture and copies
    (L1459-1500). Streaming textures convert into the locked staging memory.
  - The tests use the software renderer, which takes RGBA8888 natively
    (`SDL_render_sw.c` L1088-1089), so they never see this.
- **Fix.** Switch the packed layout to `0xAARRGGBB` (`ARGB8888` =
  `B8G8R8A8_UNORM`, the swap-chain format). It's mechanical: the
  `pack_rgba` / `red` / `green` / `blue` / `alpha` helpers, the sites above,
  `Png.cpp` L34 and L65, `Screenshot.cpp`, and the test canvases
  (`HdCompositorTests.cpp` L35, L97, L333). Add a debug assert that no
  engine texture has a native child (`SDL_PROP_TEXTURE_PARENT_POINTER`).
- **Gain** (est.). One malloc, one swizzle and one free less per upload on
  every GPU backend: about 30-50% on top of each D3D11 upload, i.e. 1.5-4 ms
  of the measured 4-10 ms text upload, and 0.1-0.3 ms per redrawn layer.
- **Risk.** Low (channel mistakes show at once in the GPU-vs-CPU tests).
  **Effort:** S.

### P2. Atlas flush sends the full page width times the changed rows; pictures keep empty rows
- **Problem.** `flush_atlas` uploads `{0, dirty_y0, page_w_, dirty_y1 -
  dirty_y0}` (`HdCompositor.cpp` L281-282). At k=6 a page is 4096x2048
  (`size_atlas` L81-93): 32 MiB on the GPU plus a 32 MiB CPU copy (L110). New
  lines land on any shelf that fits (L141-155), so the span covers earlier
  screens' shelves too. That is the measured 5.7 MB sent for 3.4 MB; the worst
  case is a whole page for a few KB. Line pictures are also the full font
  box (`HdTypeface.cpp` L365-372) rounded out to engine pixels. The
  capitals-only fonts (SUPERSMA, CSTANDAR, MINI) have no descenders, so about
  20-30% of each of their pictures is empty (est.).
- **Fix** (still needed for narrowed and non-ASCII lines). Keep a dirty
  x-range per shelf, merge rects only when the waste is under about 256 KB
  (bytes against per-call staging cost), and upload each merged rect. Trim
  coverage to the rows that hold ink in `HdTypeface::line`, keeping
  `baseline` as an offset.
- **Gain** (est.). Sent ≈ picture bytes, minus 20-30%: text upload −40-60%
  for fallback lines. **Risk:** low (`HdTextTests`). **Effort:** S.

### P3. Finish the glyph sheets: keep the per-letter cost off the CPU
The design is right; SDL_ttf's renderer text engine and modern UI renderers
work the same way. These gaps remain:
- **One `HdCmd` and one `SDL_RenderTexture` per letter, twice with a shadow**
  (`Font.cpp` L413-445, `HdCompositor.cpp` L562-588). 1,500 letters make
  about 3,000 commands, each a `shared_ptr` copy, two texture state calls and
  an SDL command with 4 vertices: about 0.3-0.6 µs each, so 1-2 ms per redraw
  of a text-heavy layer (est.). That is a new steady cost in place of the old
  one-off upload. **Fix:** in `draw_layer`, merge consecutive commands with
  the same texture and scale mode and no base run between them into one
  `SDL_RenderGeometry` call (per-vertex colour = tint x fade, one index
  buffer). `HdList` semantics stay as they are. Integer, texel-aligned quads
  with NEAREST rasterize exactly as `SDL_RenderTexture` does, and the
  GPU-vs-CPU test confirms it. A single "text run" `HdCmd` per line later
  would also shrink the lists and the `compact_if_needed` work.
- **Narrowed lines and non-ASCII letters fall back to pictures** (L323,
  L349). **Fix:** quantize the squeeze to 1/32 steps and build those sheets
  lazily on the prewarm thread. Extend sheets to Latin-1 (32-255), about 2x
  the area.
- **Coverage is still assembled for sheet lines** (`HdTypeface.cpp` L373-395)
  and kept in the 4,096-line cache. Neither `materialize()` nor the GPU needs
  it (both sample the sheet with the tint). **Fix:** skip it when
  `from_sheet`.
- **Glyph cache keyed on exact float scale bits** (L47-62): every squeeze
  factor rasterizes again. The quantization fixes this.
- **Gain** (est.). Text upload about 0 after the intro; text draw under 0.3 ms
  per layer; the 128 MiB picture cache (`Font.cpp` L126) and atlas growth (M4)
  go away. **Risk:** low-medium (shadow → letter is premultiplied "over",
  which is associative: equal within 1/255). **Effort:** S (geometry), M (all).

### P4. 4K render targets for 8 mostly empty overlays, destroyed and re-made on every screen change
- **Problem.** Each layer gets a target of its area x k. At k=6 the 8
  full-screen overlays take 35.2 MiB each (281 MiB). A paneled screen adds
  the border 35.2, main 18.6, lit main 18.6, input 3.5, status 4.0, back and
  display 35.2 each: about 432 MiB before the pool, about 1.1 GiB at k=8.
  `clear_layers` gives back every texture, even those of overlays that persist
  (`destroy_layer_textures` L55-65). `give_back` keeps 6 and destroys the
  rest (L90-99), so each screen change creates 6-7 targets of 35 MiB, each
  costing milliseconds (`Renderer.h` L91-93). `gpu_cached_.clear()` (L64) also
  re-renders every overlay.
- **Fix.** Keep the textures of views still in the new layer list (the
  overlays have stable `Renderable*`). Set `max_pooled_targets` to at least
  14. Create targets only for layers with content (P6b), later sized to it.
- **Gain** (est.). −2-6 ms per in-game screen open (not visible in the
  bench); about −250 MiB of VRAM when the overlays are empty. **Risk:** low.
  **Effort:** S.

### P5. One shared base texture, re-created at each layer size change; RGBA expanded on the CPU
- **Problem.** `upload_base` destroys and re-creates the streaming base
  texture whenever the layer size differs from the last (`HdCompositor.cpp`
  L457-470). A paneled screen goes 640x400 → 384x352 → 166x154 → 166x176 →
  640x400, so `redraw_all` and every screen open re-create it 4-5 times per
  frame, and normal play at least twice when the main and status panels are
  both dirty. Each update also runs a 256,000-entry CPU LUT (L473-487), sends
  1 MB through a streaming lock (a new D3D11 staging texture per lock,
  `SDL_render_d3d11.c` L1788-1810, plus P1), and flushes SDL's command queue
  because `base_` was just used (`SDL_render.c` L2351, L340-347).
- **Fix.** *Now:* create `base_` once at 640x400 and update `{0,0,w,h}`. The
  source rects in `draw_layer` / `draw_runs` already start at 0,0, so the
  output is identical. *Next:* `SDL_PIXELFORMAT_INDEX8` with
  `SDL_SetTexturePalette` (SDL 3.4, `SDL_render.h` L1018; D3D11 palette
  shader `SDL_render_d3d11.c` L2168-2183): entry 0 = (0,0,0,0), fade by colour
  mod. That is 256 KB instead of 1 MB per layer, and no CPU LUT.
- **Gain** (est.). First frame −1 to −3 ms; steady −0.5 to −2 ms when several
  panels redraw; lower p99. **Risk:** low (INDEX8: check D3D12, Vulkan and the
  software renderer). **Effort:** S (S-M with INDEX8).

### P6. Fades re-render every layer on every frame
- **Problem.** While a fade runs, `gpu_cached_.clear(); force_recompose_ = true`
  (`Renderer.cpp` L205-208). Every view's `render()`, `upload_base` and
  `draw_layer` runs each frame for 2 x 12 ticks (`default_fade_length`,
  `DisplayConstants.h`), only to apply one fade factor (LUT L473-481, colour
  mod L570-579).
- **Fix.** Draw the layers once. Apply the fade at composite time with
  `SDL_SetTextureColorModFloat(shown, f, f, f)` on each layer texture except
  `ignore_fade()` ones (`GameInfoLayer.h` L25). Premultiplied textures with
  RGB scaled and alpha kept are exactly the faded layer, so results are equal
  within 1/255 rounding. The CPU path can keep its current code.
- **Gain** (est.). A fade frame costs about 0.3 ms (composite only) instead of
  a full redraw: 3+ ms on the reference PC, 10+ ms on low-end CPUs. Fades get
  smooth on weak machines.
- **Risk.** Low-medium: check `ignore_fade` ordering and the
  `PLEGACY_CPU_COMPOSE` equivalence test. **Effort:** S-M.

### P6b. Empty layers are still uploaded, drawn and composited
- **Problem.** An empty overlay (tooltip, pause, console, debug, tutorial,
  turn lock) still costs a 1 MB base upload and a full-screen quad when dirty.
  Every frame in which a lower layer changes, each overlay is also blended
  full-screen into `back_tex_` (L232-268). That is 8 x 9.2 Mpx, about
  74 Mpx of blending per frame at 4K.
- **Fix.** After `view->render`, record content bounds: the union of non-zero
  index rows and columns (an SSE2 scan of 256 KB, about 20 µs) and the
  `HdCmd` rects. Skip empty layers entirely, and composite only the bounds.
  This also makes C1 cheap.
- **Gain** (est., bandwidth bound). GPU −0.9 ms per frame on a 3090 Ti,
  −2.5 ms on a 3060, −10 ms or more on iGPUs and Steam Deck at 4K.
- **Risk.** Low. **Effort:** S-M.

### P7. Sprites decoded again on every draw; HD pack keys rebuilt each time
- **Problem.** `Bob::frame` copies the frame, doubles it (or runs MMPX) and
  calls `HdPack::bob_frame` (`Bob.cpp` L36-71), which builds a
  `std::filesystem::path` (`HdPack.cpp` L106), lower-cases, `snprintf`s
  (L132), concatenates the cache key (L231), locks a mutex and hashes a
  string. This happens for every guest part every frame in
  `CharacterRenderer::frame_for` (L39-44), `GuestWalkScene.cpp` L595,
  `AttackScene.cpp` L541-580 and `AnimatedStats` (L840-984). Only
  `BobRenderer` caches (`BobRenderer.cpp` L31-42).
- **Fix.** Cache decoded frames per `(frame, palette)` in `Bob` and return
  `const Surface&` (callers that edit use `clone()`, which keeps HD sharing);
  cache the pack key per `(sheet, frame)`.
- **Gain** (est.). −0.3 to −1 ms per busy restaurant redraw, less churn.
  **Risk:** low (frames are immutable). **Effort:** S.

### P8. Remaining small costs per text line
- **Problem.** `prepare_hd_line` builds a heap `std::string` key per line per
  redraw (`Font.cpp` L238-247). The picture fallback still makes a temporary
  `Surface` plus a per-pixel `blit` (L450-452).
- **Fix.** Hash `(font, k, width, string_view)` without allocating. Use
  `hd_overlay` for the picture path too, which the new code already provides.
- **Gain.** About 0.1-0.4 ms on text-heavy screens (est.). **Risk:** low.
  **Effort:** S.

### P9. A new 1 MB `HdList` floor for every redrawn layer
- **Problem.** `Surface::fill` drops the list (`Surface.cpp` L279-284); the
  first HD draw makes a new one, and `ensure_floor` fills 256,000 `uint32_t`
  (`HdList.cpp` L157): 1 MB per full-screen layer, above the size where
  Windows switches to `VirtualAlloc`, so about 250 fresh page faults per
  redraw. `hd_transfer` allocates two vectors per blit (`Surface.cpp`
  L199-200). `hd_clear_pixel` runs `hd_mut()` (use_count check plus
  `reset`) for every pixel in `blit` / `blit_stencil` (L424, L451).
- **Fix.** Keep the layer's list and floor and reset them in place when
  `use_count()==1`; reuse the `hd_transfer` vectors; one `plain_rect` per run
  instead of per-pixel clears.
- **Gain** (est.). −0.2 to −0.5 ms per fully redrawn frame, lower p99.
  **Risk:** low-medium (copy-on-write; `SurfaceHdTests`). **Effort:** S-M.

### P10. The run scan calls out-of-line functions per pixel and makes one-row runs
- **Problem.** For every non-leaf layer, `draw_layer` calls `floor_of()` and
  `needs_base()` per pixel (`HdCompositor.cpp` L531-546). These are defined in
  `HdList.cpp` L120-135, and there is no LTO (B1), so 256,000 x 2 calls go
  through per layer. Each run is 1 engine pixel tall, so it becomes one
  `SDL_RenderTexture` (L491-505).
- **Fix.** Scan `floor_data()` directly with a fast skip over floor 0 /
  no-base words. Merge runs with the same `(floor, x0, x1)` vertically into
  rects before the `stable_sort`.
- **Gain** (est.). −0.3 to −1 ms per redrawn non-leaf layer, and far fewer
  SDL commands. **Risk:** low (the draw-list tests compare). **Effort:** S.

### P11. A cold pack image
A pack file missing from the warm list is decoded and resampled **on the main
thread inside views** (`HdPack.cpp` L250), and the first session has no warm
list at all. See L1.

### Options weighed for screen-open cost

| Option | Verdict | Reasoning |
|---|---|---|
| GPU glyph atlas, tinted quads (P3) | **Yes. In progress; finish with geometry batching** | Removes text uploads and picture building, and keeps atlas memory constant. Per-letter draw cost must be batched. |
| Line pictures in a shared atlas | Keep as fallback only (P2) | Cost grows with the new text on each screen. Fix the dirty rects and trim. |
| Trim pictures to ink | Yes, S (P2) | The sheet slots are already ink-tight; this only matters for the fallback. |
| Persistent staging / ring uploads | Not reachable through SDL_Renderer | Every backend makes a staging texture or transfer buffer per call, even the `gpu` renderer (`SDL_render_gpu.c` L435-485). A D3D11-only path is possible: the device from `SDL_PROP_RENDERER_D3D11_DEVICE_POINTER`, textures from `SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER`, then `SDL_FlushRenderer` and a persistent staging ring. Consider it only if uploads still dominate after P1, P2, P3 and P5. |
| SDL_GPU instead of SDL_Renderer | **Later (2.0), not now** | Gains: cycled transfer buffers, one copy pass per frame, BC7/KTX2, custom shaders (palette, one composite pass, float light map), HDR swap chain, explicit frames in flight. Costs: rewrite of Renderer, HdCompositor and LightPass (L); a shader toolchain (SDL_shadercross); no software renderer, which the GPU-vs-CPU tests rely on; GPUs limited to D3D11 lose support. Keep SDL_Renderer as the fallback when it comes. The middle path, the `gpu` render driver with `SDL_GPURenderState` (`SDL_render.h` L297-306, L384), allows custom shaders but does not fix uploads. |
| Pre-upload during transitions / fades | Partly | The first frame after a faded transition is drawn at fade 0 (`start_fade_in`), so the stall is already black. Building the next view before the midpoint is **not safe for Classic**: `run_midpoint` changes state (`Game.cpp` L579-583). Use per-screen warm lists instead (L1) to stream the destination's art while the fade-out runs and on idle frames. |
| View work on worker threads | **No** | `ResourceManager` caches (`ResourceManager.h` L23-31), the Font static caches (`Font.cpp` L137-202) and many view members are not thread-safe. Views may keep state inside `render()`. The gain after P7, P9 and P3 is at most about 3 ms; the risk to Classic determinism and stability is high. Offload pure work instead: decode, rasterization, premultiply. |

**Recommendation.**
1. P0, then P1 + P5 + P4 + P2 (about 3 days).
2. Finish P3 with geometry batching.
3. Measure cold. Expect 10-12 ms (est.) with HD text, under the 16 ms gate.
4. Plan SDL_GPU only if BC7 packs or HDR become goals.

---

## Area 2: steady-state cost and frame pacing

### C1. Soft-alpha texels build up in upper layers (a correctness bug, and a dirty-rect opportunity)
- **Problem.** `back_tex_` is never cleared. Only layers at or above the
  lowest changed one are composited again, each with `BLEND_PREMULTIPLIED`
  over the previous frame (`Renderer.cpp` L212, L232-233, L261-268). Where an
  upper layer has partial alpha (HD text anti-aliasing, soft sprite edges) over
  a region whose lower layers were *not* redrawn, it is blended again every
  frame: `out = s + (1-a)(s + (1-a)bg)`. Within about 10 frames the soft edges
  are opaque. Example: `GameInfoLayer` draws shadowed HD text straight onto a
  transparent full-screen layer over the wood border (`GameInfoLayer.cpp`
  L19-27, `ScreenText.cpp` L24-33), while in the restaurant the flickering
  lit main panel recomposites every frame. Not visible at hd_scale 1 (binary
  alpha) or in the bench (`redraw_all` recomposites everything), which is why
  the gates miss it. This is from reading the code; confirm with the test
  below.
- **Fix.** Compute the changed rect (the union of the areas, or the P6b
  bounds, of layers redrawn or relit this frame). Clip `back_tex_` to it,
  clear it (or draw the bottom layer with `BLENDMODE_NONE`), and composite
  every layer that intersects it, bottom-up. That is correct dirty-rect
  compositing, and full-screen overlays then cost only the changed area. The
  CPU path needs the same change (L357-414).
- **Test.** Static layer 0; layer 1 changing each frame in another rect;
  layer 2 with one alpha-128 texel over layer 0. After 10 frames, compare
  with a full recomposite.
- **Gain.** Correct anti-aliasing; GPU fill from about 100 Mpx per frame to
  the changed area. **Risk:** low. **Effort:** S-M.

### S1. Every presented frame copies the full 4K frame once more than it needs to
- **Problem.** `back_tex_` → display texture with the cursor (L292-293), then
  the display texture → back buffer (L326). That is 9.2 Mpx (74 MB of traffic)
  extra per frame.
- **Fix.** On the GPU path, draw `back_tex_` and then the cursor straight to
  the back buffer. `with_current_frame` already reads `back_tex_`.
- **Gain** (est.). −0.1 ms per frame (3090 Ti) up to −1 ms (iGPU).
  **Risk:** low. **Effort:** S.

### S2. The base layer is drawn under fully opaque HD backgrounds
- **Problem.** Step 1 always draws the whole base (L516-520), even when a leaf
  picture covers every block opaquely.
- **Fix.** Skip step 1 for `is_leaf()` when `leaf_opacity()` is all ones;
  cache that per image.
- **Gain.** One full-layer fill less per background redraw. **Effort:** S.

### F1. Frame pacing: 30/60/120 caps, a sleep after a blocking present, input read early
- **Problem.** `FpsOption` offers only 30, 60 and 120 (`GameOptions.h` L18);
  the defaults are 60 with vsync on (L185, L195). With vsync on, Present blocks
  (frame latency 1, 2 buffers, flip-sequential: `SDL_render_d3d11.c` L712,
  L885-895), and then `FpsManager` also sleeps to its own grid (`Game.cpp`
  L655). Without VRR, 60 fps on 144 Hz shows each frame for 2 or 3 refreshes in
  turn: judder, which breaks `smooth_motion`. Events are polled before the
  work and the sleep comes after present: up to one frame of extra latency.
- **Fix.** Default to a "Display" cap from
  `SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow)` (exact
  numerator/denominator), plus 144/165/240 and "Unlimited". With vsync on,
  don't sleep. With vsync off on VRR, cap at refresh − 3%. Sleep *before*
  `handle_events` (late input sampling). When minimized or occluded, keep
  ticking but skip `render()`.
- **Gain.** No judder at any refresh rate, about one frame less latency, less
  power. **Risk:** low (tick count and order unchanged). **Effort:** S.

### F2. The interpolation clock has millisecond resolution
- **Problem.** `GameClock` uses `SDL_GetTicks()` in ms (`Game.cpp` L452-453,
  `GameClock.h` L28-44), and `tick_alpha` comes from an accumulator in ms
  (`Game.cpp` L617-620). At 240 Hz, dt alternates between 4 and 5 ms, about
  12% jitter in gliding guests.
- **Fix.** Keep the ms tick scheduler exactly as it is, so the tick cadence and
  `hd_gates.py` fingerprints don't change. Compute `tick_alpha` from
  `SDL_GetTicksNS()` measured since the last tick.
- **Risk.** Low. **Effort:** S.

### F3. The stream budget is a fixed 3 ms per frame, and a band can overrun it
- **Problem.** `stream_budget_ms = 3.0` (`Renderer.h` L87) is checked before a
  band of about 1M texels (`HdCompositor.cpp` L348, L371), so one call can take
  about 5 ms (est.). At 240 Hz (4.2 ms per frame) that cuts the frame rate.
- **Fix.** Budget = clamp(target frame time − last frame cost − 1 ms,
  0.25, 3) ms. Do the premultiply and swizzle on a worker; keep only the GPU
  copy on the main thread.
- **Effort.** S.

### S3. Dirty-rect / partial redraw status
- **Now.** Dirty state is per layer only. A dirty layer is drawn in full, and
  every layer above a changed one is composited in full.
- **Recommended.** C1 + P6b give rect-level compositing.
- **Not recommended.** Sub-rect invalidation *inside* views: an L effort,
  touching every view, with risk to faithful drawing.

---

## Area 3: memory

### M1. The HD pack cache keeps every decoded picture for the whole session
- **Problem.**
  - `HdPack::cache_` holds strong `shared_ptr`s and is never trimmed
    (`HdPack.h` L128, `HdPack.cpp` L221). Each full-screen picture at k=6 is
    35 MiB of straight RGBA.
  - A long session that visits most screens holds the art it has seen: 145
    VGA pictures and up to 1,554 BOB frames, potentially 3-5 GB (est.).
  - Because these strong refs keep everything alive, `HdCompositor::end_frame`
    never sees a picture expire (L424-431). GPU memory is bounded only by the
    fixed 1.5 GiB LRU (`HdCompositor.h` L173).
- **Fix.**
  - Add an LRU over strong refs with a byte budget (default 1 GiB at k ≤ 6,
    scaled by k², configurable); evicted entries become weak.
  - Keep the 1-byte-per-engine-pixel opacity tables, which are cheap.
  - With the decoded cache from L3, evicting costs little.
  - Base the GPU budget on adapter memory (DXGI `QueryVideoMemoryInfo` via
    the SDL D3D11 device property) or a setting.
- **Risk.** Medium: a re-decode is a hitch if L3 isn't done. **Effort:** M.

### M2. Renderer layer surfaces are never erased (a leak, and stale reuse at the same address)
- **Problem.** `layer_surfaces_` is keyed by `Renderable*` (`Renderer.h` L120,
  `Renderer.cpp` L218, L366) and is never cleared (`clear_layers` L510-520).
  - Every screen visited leaves its surfaces behind: at least 256 KB of
    indices per layer, plus the `HdList` floor (1 MB) and strong refs to the HD
    pictures it drew.
  - A new view allocated at a freed view's address inherits the old surface.
    With `preserve_background` (no `fill`, L235-238) it would show the old
    view's pixels.
- **Fix.** In `clear_layers*` / `add_layers`, erase the entries that are not
  in the new layer list.
- **Gain.** Bounded memory over long sessions; the stale-content case is gone.
  **Risk:** low. **Effort:** S.

### M3. VRAM use, and a fatal exit when it runs out
- See P4 for the numbers: about 432 MiB of targets plus a pool of up to 211 MiB
  plus 1.5 GiB of art at k=6; about ×1.8 at k=8.
- Every creation failure is `fatal()` → `std::exit(1)` (`Renderer.cpp`
  L79-81, L133-135; `HdCompositor.cpp` L463-465; `LightPass.cpp` L57-59,
  L75-77).
- **Fix.** P4 + P6b. On failure, evict the LRU art and retry. Otherwise lower
  k or fall back to the CPU compositor with a message (R2). **Effort:** S-M.

### M4. Atlas fragmentation; float resampler buffers
- Atlas slots are never reused: a page is refilled only when every picture in
  it has gone (`evict` L399-421), atlas entries skip the LRU (L436-437), and
  each page keeps a 32 MiB CPU copy (L110). P3 makes the atlas a fallback;
  otherwise add per-shelf free lists or compact at screen changes. **S.**
- `gfx::resample` premultiplies into 16 B/texel float planes (`HdImage.cpp`
  L24-44, L199-219): about 600 MB peak for 5120x3200 → 3840x2400, on one
  thread. It always does x first, despite the comment at L207. Use
  `stb_image_resize2` (SIMD, premultiplied) in row strips on the pool, or
  better, avoid runtime resampling (L3). **S-M.**

### M5. shared_ptr churn, and the bounded caches: low priority
- `HdCmd` copies in `hd_transfer`, `weak_ptr::lock` per draw
  (`HdCompositor.cpp` L306, L356), now one per glyph: about 10-20 ns each,
  under 0.1 ms per frame before the sheets. P3's batching removes the
  per-glyph part; intrusive or non-atomic counting isn't worth the risk.
- Bounded and fine: letters 4,096 lines, pictures 128 MiB (`Font.cpp`
  L125-126), glyphs 4,096 (`HdTypeface.cpp` L45).

---

## Area 4: threading

### T1. The logger is not thread-safe, and the console sink dangles after `run()`
- **Problem.** `Logger::log` changes static `last_time_` and walks `sinks_`
  with no lock (`Logger.cpp` L41-67). The prefetcher logs from its own thread
  through `load_png` (`Png.cpp` L28-30, L35-37) into the `DebugConsole` sink.
  That sink captures a `DebugConsole&` local to `Game::run` (`Game.cpp` L287,
  L295; `DebugConsole.cpp` L114-116) and is never removed, so any log line
  after `run()` returns (static destructors, a prefetch still running at exit,
  `HdTypefaces`) is a use-after-free.
- **Fix.** A mutex in `log`; RAII sink handles removed when their scope ends;
  background threads queue lines that the main thread drains into the console
  once per frame. **Risk:** low. **Effort:** S.

### T2. Exceptions during decode break the promise or terminate the process
- **Problem.** `decode()` can throw: `bad_alloc` in `resample`, or
  `filesystem` errors. In `load()` (L250) the pending entry is then never
  erased, and later lookups get `std::future_error` from `in_flight.get()`
  (L248). In the prefetch `jthread` (L271-303) the exception escapes and calls
  `std::terminate`.
- **Fix.** Catch in `decode`, record `Rejected` with the message, and always
  `set_value` (or `set_exception`) and erase `pending_`.
- **Effort.** S.

### T3. One prefetch thread for the whole warm list
- **Problem.** Decoding is sequential (L271-303). At about 150-400 ms per 4K
  file (stb_image plus resample, est.), a 1.5 GiB warm list takes
  6-15 s, often longer than the intro.
- **Fix.** A persistent pool with N−2 workers, using
  `SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW)` and the same budget.
  Check the stop token inside the resample row loop.
- **Gain.** Warm-up 4-8x faster on 8-core and 16-thread CPUs. **Effort:** S-M.

### T4. Threads spawned per call
- **Problem.** `upload_rows` (L250-269) and `to_sdl_surface` (L628-649) create
  up to 7 `jthread`s per call, at about 20-50 µs each on Windows. That adds
  jitter to p99.
- **Fix.** Use the same persistent pool, with a `std::latch` per job.
- **Effort.** S.

### T5. Shutdown order
- **Problem.** `HdPack` and `HdTypefaces` are function-local statics. Their
  threads are joined during static destruction, *after* `~Game` has run
  `SDL_Quit` (`SDLSession.h`). A prefetch still running at exit calls
  `SDL_LoadPNG` and `SDL_GetError` after `SDL_Quit` (TLS has been torn down).
- **Fix.** Stop and join explicitly at the end of `Game::run`: call
  `HdPack::instance().configure({})` (which runs `stop_prefetch`), and
  request stop on the prewarm workers. Unregister the log sinks (T1).
- **Effort.** S.

### T6. Data races in audio; one prewarm thread per font
- `music_volume_`, `sound_volume_`, `audio_enabled_` and `music_state_`
  (`AudioManager.h` L151-162) are written on the main thread and read in the
  callback (`AudioManager.cpp` L292, L325, L330) without atomics;
  `is_playing()` reads `music_state_` unlocked (L275). **Fix:**
  `std::atomic`. **Effort:** S.
- `HdTypeface::prewarm` starts a thread per call (L229-249): 7 at once in
  `load_game_data` (`Game.cpp` L1107-1116). Fine on a 5800X3D, competes with
  the intro on 4-core CPUs. **Fix:** low-priority tasks on the pool (T3).

**`std::jthread` use** is modern and correct (stop tokens; the prewarm workers
are declared last in `Impl` so they are destroyed first, `HdTypeface.cpp`
L93-95). Only T3/T4 (pooling) and T5 (shutdown order) need work.

---

## Area 5: loading and startup

### L1. The first session decodes 4K art on the main thread (100-500 ms stalls)
- **Problem.** The warm list only exists from the second session
  (`Game.cpp` L200-206, `HdPack.cpp` L306-339). Every new screen in the first
  session decodes and resamples synchronously inside `views` (`HdPack.cpp`
  L250). The same happens later for any file missing from the list.
- **Fix.**
  - Ship a default warm list with each pack (`hdpack.py` can write one from
    the manifest in screen order), and tag entries with the screen that used
    them (`ScreenTag` / `UIState` kind).
  - At start-up, prefetch in priority order on the pool (T3).
  - When a transition is requested (`pending_fade->requested()` or
    `ui().ui_changed()`), move that screen's entries to the front.
- **Gain.** No multi-frame stalls on first runs. **Risk:** low.
  **Effort:** S-M.

### L2. The PNG decoder is stb_image, with two extra full passes
- **Problem.** `SDL_LoadPNG` is stb_image inside SDL (`SDL_stb.c` L45-59,
  L381-388) through IO callbacks, followed by `SDL_ConvertSurface` to RGBA8888
  (`Png.cpp` L34) and a `memcpy` into `HdImage` (L44-49).
- **Fix.** Wuffs: about 2-3x faster than libpng/stb_image, memory-safe and
  fuzzed (packs are shared online). Decode straight into `HdImage::pixels` in
  the final layout (`BGRA_NONPREMUL` = ARGB8888 after P1). libspng is the
  simpler choice. fpng decodes only fpng files: use it for L3's cache, not for
  packs.
- **Gain** (est.). 2-3x faster decode, two passes and one allocation fewer.
  **Effort:** M.

### L3. Resampling at run time, and no decoded cache
- **Problem.** A pack not exactly 640k x 400k is resampled on every load
  (`HdPack.cpp` L189-191). `hdpack.py`'s default `--scale 6` makes 1920x1200
  (`HD_REMASTER.md` L186-188), so every 4K user (k=6) pays a float upscale on
  every load.
- **Fix.** A decoded cache in the pref folder, key (path, mtime, size, k) →
  raw ARGB + zstd (about 1-2 GB/s) or QOI/fpng, written by the pool after the
  first resample. `hdpack.py` should warn when the pack doesn't match k.
- **Gain** (est.). Second-session loads 5-10x faster; M1 eviction gets cheap.
  **Effort:** M.

### L4. GPU-compressed art (BC7 in KTX2 with zstd)
- 4x less VRAM and upload and almost no decode, encoded offline by the hdpack
  tools. But SDL_Renderer has no compressed formats (only SDL_GPU does), and
  the CPU still needs texels for `opaque_block`, `materialize` and
  `blit_half` (ship opacity tables in the KTX2 metadata; keep PNG for the CPU
  paths). **Verdict:** together with SDL_GPU. **Effort:** L.

### L5. Pack layout and file-system calls
- **Problem.** There are 1,699 loose files. Every lookup stats the file,
  including the many misses (`HdPack.cpp` L169). On Windows each `CreateFile`
  goes through Defender (about 0.1-1 ms).
- **Fix.**
  - Index the pack once in `configure` (one recursive `directory_iterator`
    into an `unordered_set`).
  - Optionally a single-file archive (a stored zip or `.pak` with an index).
  - Per-sheet BOB atlases, so the sprites of one sheet share a texture and
    SDL batches them into one draw call
    (`SDL_render_d3d11.c` L2714-2733 merges consecutive same-texture copies).
- **Effort.** S (index), M (archive or atlases).

### L6. Audio set-up blocks the intro on the main thread
- **Problem.** `Intro::tick` runs `audio->init()` and `load_default_assets()`
  (adl_init plus decoding every VOC) synchronously (`Intro.cpp` L105-111,
  `BusyCursor.cpp` L42-51). No events are pumped while it runs.
- **Fix.** Run it as a task on the pool. Keep the intro holding on the logo
  until it is done, so the order of what the player sees is unchanged.
- **Effort.** S.

---

## Area 6: audio

### A1. `mix_sounds` overwrites instead of mixing
- **Problem.** `buffer[i] = sound.samples[...]` (`AudioManager.cpp` L440-444):
  the last active slot overwrites the others. When it is shorter, the tail of
  the buffer keeps the previous sound's samples. The comment says "additive".
- **Fix.** First decide which model is faithful. The DOS Sound Blaster DSP
  plays one digital voice at a time, so check in DOSBox whether the original
  layers VOCs.
  - If it plays one voice: make that explicit, with the newest or a priority
    order, and clear the tail.
  - If not: mix additively in the float `mix_buffer_`.
- **Risk.** Behaviour (audio only), so verify. **Effort:** S.

### A2. A looping sound restarts at 0, not at `start_offset`
- **Problem.** A looping sound goes back to 0 (L426-430), so the chopper's
  skipped static (L137-144) returns on every loop.
- **Fix.** Restart at `start_offset`. **Effort:** S.

### A3. The audio callback holds the music lock for the whole OPL render
- **Problem.** `mix_music` keeps `music_mutex_` locked while `adl_play` runs
  the Nuked OPL3 emulator (L343-363; Nuked is chosen in `CMakeLists.txt`
  L268). `play_music` and `stop_music` on the main thread wait for it, a
  possible frame stall of up to one callback period.
- **Fix.** A lock-free SPSC command queue to the audio thread. Volumes become
  atomics (T6). **Effort:** S-M.

### A4. Fixed 44.1 kHz and default buffer; hard clipping
- **Problem.** The stream opens at 44.1 kHz (`AudioManager.h` L147), so SDL
  resamples for the usual 48 kHz Windows device, with the default buffer
  size. Music is boosted ×6.0 and then clamped (L389-396, L334-338).
- **Fix.** Open at the device rate (`SDL_GetAudioDeviceFormat`); libADLMIDI
  renders at it directly, and VOCs are resampled once at load with an
  `SDL_AudioStream` instead of `resample_linear`. Set
  `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES` to about 512. Add a soft-knee limiter.
- **Effort.** S.

**Already fine.** `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)`
follows changes of the default device. The mix buffers are reused.

---

## Area 7: window, display and OS integration

### W1. No automatic HD scale; the window starts at a fixed pixel size
- **Problem.** `hd_scale` defaults to 1 (`GameOptions.h` L183). The window is
  640x400 or 1280x800 real pixels (`window_size_for`, `Game.cpp` L886), which
  looks tiny on a 4K display at 150%. SDL is per-monitor DPI-aware v2 by
  default (`SDL_windowsvideo.c` L573-576), but the engine never reads
  `SDL_GetWindowDisplayScale`.
- **Fix.**
  - Add `hd_scale = auto`, the default for the HD build:
    k = clamp(ceil(display_h / 400), 2, 8), capped by VRAM (M3).
  - Size the window from `SDL_GetDisplayUsableBounds`: the largest 16:10 fit
    of about 85%.
  - Save and restore the window's position, size and display.
- **Risk.** Low. **Effort:** S-M.

### W2. Downscaling quality when the frame is larger than the window
- **Problem.** The final step is LINEAR (`Game.cpp` L1209). At k=6 on a
  1440p display (2400 → about 1440, ×0.6), bilinear minification aliases.
- **Fix.** Auto-k (W1) keeps the ratio between 0.85 and 1.2. Otherwise
  downscale in two steps, or use `SDL_SCALEMODE_PIXELART` when upscaling.
- **Effort.** S.

### W3. 1440p, 8K and aspect ratios
- 1440p: k=4 (2560x1600) is right. 8K: the maximum k=8 gives 5120x3200,
  upscaled ×1.35; fine to look at, but targets need about 1.1 GiB (P4) and art
  about 64 MiB per screen. Gate k=8 on VRAM; keep `max_hd_scale = 8`.
- 16:10 is native; 16:9 and 21:9 are letterboxed by
  `SDL_SetRenderLogicalPresentation(..., LETTERBOX)` (`Game.cpp` L923-925,
  L544-546); 4:3 CRT works. Add an "integer scale" option and optional
  pillarbox art. `enforce_aspect_ratio` (L1127-1181) fights Windows snapping:
  allow free resizing and letterbox instead.

### W4. HDR output (opt-in)
- SDL 3.4 supports scRGB output (`SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER`
  = `SDL_COLORSPACE_SRGB_LINEAR` on D3D11, D3D12 and Metal,
  `SDL_render.h` L286-292).
- Keep the art at SDR white (`SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT`). Only
  the light pass goes above 1.0: an `RGBA64_FLOAT` light map and lit target,
  which also removes the banding of the 8-bit map
  (`LightPass.cpp` L73-79). The CHANGELOG's "lights can be brighter than the
  art" is not possible with the current 8-bit MOD map.
- Off by default, so Classic keeps its look. **Effort:** M (L with SDL_GPU).

### W5. Fullscreen, alt-tab and multiple monitors
- `SDL_SetWindowFullscreen(true)` with no mode set is borderless desktop
  fullscreen (`Game.cpp` L1119-1123). On Windows 11 with the flip model this is
  the right choice: instant alt-tab, independent flip, and VRR with tearing
  allowed (`SDL_render_d3d11.c` L635-637, L993-998). Keep it; exclusive mode
  isn't needed.
- **Missing.**
  - Handling of `SDL_EVENT_WINDOW_DISPLAY_CHANGED` and
    `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`: re-run auto-k for the window
    size and cursor scale.
  - A remembered fullscreen display.
  - A "mute when inactive" option.

### W6. Dual-GPU laptops composite on the iGPU; no manifest
- **Problem.** Nothing exports `NvOptimusEnablement` /
  `AmdPowerXpressRequestHighPerformance` (no match in `src` or
  `deps/src/SDL/src`), so D3D11 takes the default adapter and Windows picks
  power saving. `assets/Pizza Legacy.rc.in` has no manifest.
- **Fix.** Two `extern "C" __declspec(dllexport)` globals in `Main.cpp`. A
  manifest declaring PerMonitorV2 before any window (see the caveat at
  `SDL_windowsvideo.c` L525-531), `activeCodePage UTF-8`, `longPathAware`
  and supportedOS.
- **Gain.** The discrete GPU on Optimus and Enduro laptops. **Effort:** S.

### W7. Software cursor latency
- **Problem.** The default software cursor is drawn into the frame
  (`Renderer.cpp` L294-322), so it lags by the full frame pipeline.
- **Fix.** A hybrid: a hardware cursor built from the HD image at the window's
  DPI (`SDL_CreateColorCursor`) for the pointer, and the software cursor only
  while dragging or placing an object, which the CHANGELOG gives as the
  reason for it. F1 also helps.
- **Effort.** M.

---

## Area 8: robustness

### R1. Device lost or reset is not handled
- **Problem.**
  - When the D3D11 device is removed (TDR, driver update, GPU switch, some
    RDP cases), SDL releases every texture (`D3D11_ReleaseAll` L338-345) and
    posts `SDL_EVENT_RENDER_DEVICE_RESET` or `_LOST` (L1100-1115).
  - The engine handles no `SDL_EVENT_RENDER_*` event (`Game.cpp` L755-802).
    Every cached `SDL_Texture*` (compositor cache, atlas pages, pool,
    `back_tex_`, display texture, light pass, cursor) is left dead, and the
    screen stays black or wrong.
- **Fix.**
  - On `DEVICE_RESET`: `gpu_.reset()`, `light_.reset()`, destroy the pools
    and the back, display and cursor textures, `sdl_.texture.reset(create_display_texture())`,
    `invalidate()` and `redraw_all()`. The HD art is still in RAM, and atlas
    pages rebuild from their CPU copies.
  - On `TARGETS_RESET`: `gpu_cached_.clear()`.
  - On `DEVICE_LOST`: save the warm list and show an error (R2).
- **Test.** Force a TDR with `dxcap -forcetdr` (Windows Graphics Tools), or
  add a console command that destroys and re-creates the renderer.
- **Effort.** S-M.

### R2. `fatal()` exits silently in a GUI program
- **Problem.** `fatal` writes to `std::cerr` and calls `std::exit`
  (`Fatal.h`). Started from Explorer, the GUI build has no terminal
  (`Main.cpp` L210-224), so the window just vanishes. It is also used for
  recoverable cases: texture creation, HD size mismatches (`attach_hd`),
  `fatal(SDL_GetError())` in the renderer.
- **Fix.** Split errors into fatal and degrade (CPU compositor, lower k,
  another renderer). For fatal errors in interactive runs only, show
  `SDL_ShowSimpleMessageBox`; never in `--exec` or other hidden batch runs,
  which must stay invisible. Always log to file (R3). **Effort:** S-M.

### R3. No log file, no system information, no minidumps
- **Problem.** The only sinks are stdout/stderr (invisible in the GUI build)
  and the in-game console (`Game.cpp` L139-147, L295); release builds log
  warnings only (`Logger.cpp` L14-22). Nothing catches crashes.
- **Fix.** A rotating `plegacy.log` in the pref folder. At start-up, log the
  version, renderer (`SDL_PROP_RENDERER_NAME_STRING`), GPU and driver (DXGI
  adapter), displays (mode, refresh, scale, HDR headroom), k and the pack;
  per screen open, log the bench breakdown. For crashes:
  `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` (`dbghelp.dll` is
  already on the import whitelist in `build-windows-cross.sh`), plus
  `std::set_terminate` and `_set_purecall_handler`. Write `crash-<time>.dmp`
  and the last 200 log lines locally, and keep symbols per release (B4).
- **Effort.** S-M.

### R4. Renderer choice and PNG hardening
- SDL's default order (D3D11, D3D12, … software; `SDL_render.c` L110-150)
  falls back on its own, but users can't choose and the log doesn't say
  which was used. **Fix:** a `renderer = auto|direct3d11|direct3d12|vulkan|opengl|gpu`
  setting (`SDL_HINT_RENDER_DRIVER` before `SDL_CreateWindowAndRenderer`,
  `Game.cpp` L893) and a log line.
- stb_image isn't built for untrusted input, and packs are shared online.
  L2's Wuffs fixes this; until then, read the IHDR first and refuse more than
  16k per side or 256 MP.
- **Effort.** S.

**Already robust.** The pack keeps the original art for missing or wrong-aspect
files (`HdPack.cpp` L169-197); warm-list paths are checked (no `..`, not
absolute, L331-335); `hd_fonts.json` entries can't leave their folder
(`HdTypeface.cpp`, configure).

---

## Area 9: build and toolchain

### B1. No link-time optimization
- **Problem.** `CMAKE_INTERPROCEDURAL_OPTIMIZATION` is never set
  (`CMakeLists.txt` L83-133, L320-353). Hot helpers in other translation units
  can't be inlined: `HdList::floor_of` / `needs_base` and `opaque_block`.
- **Fix.** `set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)` after
  `check_ipo_supported`: GCC `-flto=auto`, Clang ThinLTO, MSVC `/GL /LTCG`.
- **Gain** (est.). 5-15% CPU in the renderer and simulation. **Risk:** low
  (longer builds). **Effort:** S.

### B2. Baseline instruction set
- **Problem.** No `-march`, so the build targets x86-64 v1 (SSE2). The
  per-texel loops (premultiply, LUT, `blend_over`, resample, Bresenham) don't
  vectorize well.
- **Fix.** Use `-march=x86-64-v2`: Windows 11 24H2 already requires SSE4.2 and
  POPCNT, so nothing is lost. Add `__attribute__((target_clones("avx2","default")))`
  (GCC/Clang) on the 5-6 hot loops. `PLEGACY_VECTORIZE_REPORT`
  (L140-157) can check the result.
- **Effort.** S.

### B3. Profile-guided optimization
- **What.** Instrument the build, run a `--exec` script (title, restaurant
  bench, city, statistics, `game tick 2000`) hidden in the local CI scripts,
  then rebuild with the profile. GCC: `-fprofile-generate` / `-fprofile-use`.
  Clang: `-fprofile-instr-*`.
- **Gain** (est.). 10-20% on the branchy simulation and view code.
- **Effort.** M (the pipeline in `scripts/ci-local.*`; no cloud CI needed).

### B4. MinGW (GCC, msvcrt.dll, winpthreads, DWARF) against llvm-mingw or MSVC
- **Now.** The cross build is `x86_64-w64-mingw32-g++-posix` with `-static`
  (`cmake/mingw-w64-x86_64.cmake`). It imports `msvcrt.dll` (the old CRT,
  allowed in `build-windows-cross.sh`) and uses winpthreads for
  `std::mutex` / `std::thread`. It has no PDBs, so minidumps (R3) are hard to
  symbolize in WinDbg.
- **Recommendation.** For releases, use **llvm-mingw** (clang + lld, UCRT,
  `-gcodeview` → PDB, ThinLTO), keeping the single static exe and the
  import-whitelist check. Or build with MSVC (`/O2 /GL /LTCG /MT`, already
  half-wired at `CMakeLists.txt` L116-133, L313-315) from `scripts/build.ps1`.
- **Gain.** Symbolized crashes, a modern CRT, faster locks (SRWLOCK), and
  better code generation for Windows.
- **Effort.** M.

### B5. Symbols, allocator, sanitizer and profiler presets
- Keep `-g` / PDB files per release next to the zip, not in it;
  `SOURCE_DATE_EPOCH` and `-ffile-prefix-map` make builds reproducible.
- Optional static mimalloc: less page-fault churn from the 1 MB floors and
  line pictures until P9 and P3 land, and better p99. Measure before keeping.
- Add `tsan` / `asan` presets to `CMakePresets.json`: the CHANGELOG says
  ThreadSanitizer was used, but there is no preset.
- An optional **Tracy** build (`TRACY_ENABLE`, zero cost when off) with zones
  in `render_gpu`, `draw_layer`, `upload_*`, `View::render` and the decode
  workers.
- **Effort.** S-M.

---

## Top 10 to do next (ranked by gain over effort)

Step 0, before anything else: **P0** (an honest bench: cold, with overlays, GPU
sync). Effort S. Without it, the gains below can't be shown.

| # | Item | Gain (est. unless noted) | Risk | Effort |
|---|---|---|---|---|
| 1 | **P1** Store and create textures as ARGB8888 (native on every backend) | −30-50% on every upload: about −1.5-4 ms of the measured 4-10 ms text upload, −0.1-0.3 ms per redrawn layer | Low | S |
| 2 | **P5** Create `base_` once at 640x400 and update sub-rects (INDEX8 + GPU palette next) | First frame −1-3 ms, steady −0.5-2 ms when several panels redraw, lower p99 | Low | S |
| 3 | **C1 + P6b** Bottom-up composite clipped to the changed rect; skip empty layers | Fixes soft edges turning opaque (bug); −60-75 Mpx of GPU fill per frame at 4K (−0.9 ms on 3090 Ti, −2.5 ms on 3060, −10 ms on iGPU) | Low | S-M |
| 4 | **P4** Keep overlay targets across screens, pool ≥ 14, create targets lazily | −2-6 ms per in-game screen open, about −250 MiB VRAM | Low | S |
| 5 | **P3** Finish glyph sheets: one `SDL_RenderGeometry` per run, quantized squeeze and Latin-1 sheets, no coverage assembly for sheet lines; **P2** exact dirty rects for the remaining atlas | Text upload about 0 (was 4-10 ms measured); text draw under 0.3 ms per layer; 5.7 MB → under 3.4 MB sent for the fallback | Low-med | S-M |
| 6 | **F1 + F2** Refresh-matched cap, no sleep with vsync, input read late, ns interpolation clock, no render when minimized | No judder on 144-240 Hz, about 1 frame less latency | Low | S |
| 7 | **L1 + T3** Default warm list per pack, warm-up per screen, parallel low-priority decode | Removes 100-500 ms stalls on first runs; warm-up 4-8x faster | Low | S-M |
| 8 | **P6** Fades by colour mod at composite (no re-render) | A fade frame drops from a full redraw (3+ ms, 10+ ms on low-end) to about 0.3 ms | Low-med | S-M |
| 9 | **R1 + R2** Device reset handling; degrade instead of `fatal`, message box in interactive runs only | No black screen after TDR or a driver update; no silent exits | Low | S-M |
| 10 | **T1 + R3** Thread-safe logger with removable sinks, log file, minidumps; **B1** LTO alongside | Fixes a use-after-free and a race; field issues become diagnosable; 5-15% CPU from LTO | Low | S |

Next in line: P7 (sprite frame cache), P9/P10 (floors and run scan), S1 (one
fewer 4K copy), W1/W6 (auto-k, discrete GPU), M2 (layer surface leak), A1/A2
(audio mix), L3 (decoded cache), B2/B3/B4 (ISA, PGO, llvm-mingw).

Expected result of items 1-5 (est.): a cold screen open with HD text at about
10-12 ms (19-26 ms measured now), a steady fully redrawn frame at about 2 ms CPU,
and much lower GPU cost on the RTX 3060 and iGPUs.

---

## Already state of the art (don't touch)

* **Game and picture kept apart**: indexed 640x400 surfaces stay the truth;
  HD is an overlay the game never reads, guarded by `hd_gates.py` (GPU vs CPU
  ≤ 2/255, identical fingerprint with `smooth_motion` on and off).
* **Fixed 30 Hz simulation with interpolation**, catch-up cap and backlog drop
  (`Game.cpp` L432, L481-613); `FpsManager` with absolute deadlines and
  `SDL_DelayNS`.
* **Shader-free GPU compositing by draw list** with exact texel-copy rules
  (`HdList.h` L28-40), premultiplied throughout, on every SDL backend
  including the software renderer the tests use.
* **Alpha-correct resampling** (premultiplied; box filter down, bilinear up).
  Only speed and memory need work (M4).
* **Copy-on-write HD sharing** and per-image opacity tables (`opaque_block`).
* **Glyph sheets with tinted quads** (in progress): right design; finish P3.
* **Warm list plus time-budgeted streaming**, and **target pooling plus shelf
  atlas**: right shapes; extend and tune (L1, T3, F3, P4, P2).
* **D3D11 presentation via SDL**: flip-sequential, 2 buffers, latency 1,
  tearing with vsync off (VRR), borderless fullscreen (instant alt-tab).
* **Per-monitor DPI v2** (SDL3 default); an audio stream that follows the
  default device.
* **`std::jthread` with stop tokens**, workers declared last in
  `HdTypeface::Impl`, ThreadSanitizer-checked prefetch.
* **Release engineering**: single static exe with an import whitelist,
  zero-warnings gate, hidden `--exec` runs, local CI gates.
* **No DLSS/FSR** (`HD_REMASTER.md` L140-142): correct for a native-res 2D game.

---

## Appendix: how to check each gain

1. P0 first: `bench cold` and `bench` in the restaurant, menu, stats and city
   on the 3090 Ti and the 3060 (vsync off, GPU sync); keep the baseline.
2. After each item compare views / compositing / present / upload and the
   `first frame text: … sent …` line (`Game.cpp` L995-999); P2 + P3 should
   bring `sent` to ≤ `bytes`, then to about 0 for sheet text.
3. C1: add the 10-frame accumulation test to `HdCompositorTests`. F1/F2: log
   present-to-present intervals for 30 s at 60/144/240 Hz (jitter under
   0.5 ms with vsync on).
4. Re-run `scripts/hd-gates.ps1` after every change.
