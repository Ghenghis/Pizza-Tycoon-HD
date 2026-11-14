# HD audit: what still reaches the screen at 1994 resolution

Static code audit (no build, no game run) of `src/views`, `src/engine` and
`src/resources` at commit 94d616e ("HD text"), with the working tree as it was
on 2026-11-14. The question for every code path: in HD mode (`hd_scale` > 1),
does detailed content reach the screen without an HD picture? Such content is
drawn with the indexed pixels scaled up by nearest neighbour.

Where it helped, the exported originals (`/home/claude/w/export`) and two
local packs (`/home/claude/w/pack6`, 6x; `/tmp/pack12`, 12x) were measured
with small read-only Python checks. The game's own `GFX/PALETTE.E` was used
for the palette numbers. Nothing was rendered.

Effort: **S** = under half a day, **M** = 1-3 days, **L** = more than 3 days.
Findings are ranked by visibility, then by effort.

## How HD is lost (the mechanisms)

HD survives only when a picture is loaded through `VgaLoader::load`
(`VgaLoader.cpp` L133-143), `Bob::frame` (`Bob.cpp` L61-69) or
`Font::render_line_onto`, and is then moved with `blit`, `blit_raw`,
`blit_masked` or `blit_half`. HD is lost in these ways:

1. **No HD source at all.** The picture is built from raw bytes that the pack
   cannot replace (the cursor array, `.pc` character files, the engine's own
   `game_location.vga`), or it is computed per pixel (city maps, hatching,
   masks, diagonal lines).
2. **Plain overdraw.** `put`, `fill_rect`, `replace_colors`, or a blit from a
   surface without HD clears the HD texels under it (`hd_clear_pixel` /
   `plain_rect`).
3. **Partly transparent HD texels inside the original outline.** When a
   source pixel is opaque, the compositor first redraws the indexed pixel
   under the HD texels unless the k x k block is fully opaque (`needs_base`;
   `HdCompositor::draw_layer` L325-348, rule documented in `HdList.h` L37-40).
   Any alpha below 255 inside the 1994 outline therefore shows the blocky
   pixel through it.
4. **Palette tricks.** HD art has fixed colours, painted from the export
   palette. Season palettes, water cycling and screens that switch palette
   after drawing all change the indexed colours but not the HD art.
5. **Pack content.** Text baked into pictures, sprites magnified with MMPX
   (pixel art by design), and atlases upscaled as one image.

## The three reported symptoms

| Symptom | Cause | Finding |
|---|---|---|
| 1. City view map is blocky | The default FAR zoom map is built with one `fill_rect` per tile and has no HD source. The DISTRICTS and CONTROL tabs are per-pixel hatching. | H4, M2 (also M4, M6) |
| 2. Paper pins on `nav stats @ 0` are blocky | The code path is HD-aware: `render_push_pin` blits stanbobs frames 26-33 with `blit_masked`. The blockiness comes from the pack. In the 12x pack (`/tmp/pack12`) the pin is an MMPX 11x14 sprite, which is hard-edged 12x12 blocks. In the AI-made pack6, 91 of the pin's 93 opaque pixels have HD blocks that are not fully opaque (alpha p10 = 205; corner pieces 35-38 p10 = 169), so the nearest-neighbour base shows through. | H2 |
| 3. Pixel text | **Baked into pictures:** the Advertising items (Flyers, Posters, Newspaper Ads, TV-Ads, plus the "Advertising" title) are in `gfx/back27`. The city view dropdown labels PLAYER, RANGE, LOCATIONS and PROPERTIES are in `gfx/back21`. **Drawn by code:** the MAP, DISTRICTS and CONTROL tabs (`CityView::render_tabs` L1126-1129, CSTANDAR) and the terminal header "INTERNATIONAL TELENET TERMINAL" / "LOADING MULTIMEDIA LIBRARY . . ." (`Terminal::draw_main_menu` L88-93, CSTANDAR). Since 94d616e these go through `prepare_hd_line` and are HD text. If they still look pixelated, the screenshot predates that commit, or `hd_text` is off, or no typeface loaded (`HdTypefaces::configure` needs `data/fonts/hd_fonts.json`). The terminal background (`gfx/back28`) and its green boxes (`gfx/blitmap5`) also carry baked decorative pseudo-text. | H5 |

## Summary table

| # | Vis. | Effort | What, where on screen | Code |
|---|---|---|---|---|
| H1 | high | S | Mouse arrow, every screen | `CursorManager.cpp` `create_cursor_surface` L20-42 |
| H2 | high | S (+M art) | Fringe around every sprite; pins and corner pieces on paper pages | pack alpha + `HdCompositor::draw_layer` L325-348; `tools/hdpack/hdpack.py` `reference_mask` L146-151 |
| H3 | high | M | Character portraits: turn card, staff cards, profile, character selection, alert/confirm popups | `RawRenderer::surface` L11-16 |
| H4 | high | M | City view FAR zoom map (the default zoom) | `CityView::render_map_zoom_far` L817-859 |
| H5 | high | M (S per picture in pack) | Text baked into pictures: Advertising menu, city dropdown labels, CAPACITY, Pizza-Charts, NEWS, PIZZA flyer, cheque | `gfx/back27`, `back21`, `back37`, `back30`, `back31`, `back26`, `back36` |
| M1 | medium | S | Flat patches over textured HD backgrounds: Purchasing sticker, Stock, Newspaper date, newspaper-ad column | `PurchasingMain::render` L31-34 and others |
| M2 | medium | S-M | City DISTRICTS and CONTROL hatching | `CityView::draw_cell_with_pattern` L1158-1216 |
| M3 | medium | M | Diagonal strokes as pixel stairs: economy graph, option checkboxes, chevrons, slider knobs | `gfx::draw::line`, `line_2x`, `fill_circle` (`Draw.cpp`) |
| M4 | medium | M | City map HD art always shows spring; water does not animate | `CityView` ctor L177-189, `update_water_palette` L1570-1579 |
| M5 | medium | M | HD art in the wrong colours where the screen palette is not the export palette (guest faces, capacity bars, Stock/Purchasing) | `GuestTalkPopup::render` L229, `AnimatedStats` L537-543, `StockMain` L33, `PurchasingMain` L29 |
| M6 | medium | M | Medium-zoom city map and restaurant overview thumbnails are capped at med1's 4x4-pixel tiles | `render_map_zoom_medium` L789-815, `StatsRestaurants` L132-147 |
| M7 | medium | L | Motion and scrolling snap to 640x400 (6 texels at 4K) | `GuestWalkScene::drawn_position` L463-480, cameras |
| L1 | low | S | Stat bars: `replace_colors` punches holes in the HD bar | `StatBar::create_stat_bar` L72-75 |
| L2 | low | S | Half-size pizza: staircase rim | `draw_half_size_pizza` L36-45 |
| L3 | low | S | Busy (padlock) cursor shows an opaque square | `BusyCursor.cpp` `icon_surface` L20-29 + `Surface::hd_transfer` |
| L4 | low | S | With `software_cursor = false`, every cursor loses HD | `CursorManager::hw_apply_cursor` L189-223 |
| L5 | low | S | Capacity screen: 18 flat 2-px gradient bands, repeated 2-px slices | `AnimatedStats::render_capacity` L572-577, L631-640 |
| L6 | low | S | First-run game-location screen | `GameLocationScreen.cpp` L70 |
| L7 | low | M | Atlases upscaled as one image, so cells bleed into each other (tile seams) | `tools/hdpack/hdpack.py` (no atlas splitting) |
| L8 | low | S | Exports with a guessed palette (typ10e, walk0, walk10) | export manifest `palette_known: false` |

---

## High visibility

### H1. Mouse arrow (every screen). Effort S

* **Where:** `src/engine/ui/CursorManager.cpp`, `mouse_cursor_data` L20-33 and
  `create_cursor_surface` L38-42. It is drawn by `Renderer::render_gpu`
  L206-255 (`to_sdl_surface(*cursor_sprite, 1.0f, scale_)` L232-233) and on
  the CPU path by `draw_software_cursor` L423-439.
* **Why HD is lost:** the arrow is an 11x11 array copied from the original
  executable. It goes through `Surface::from_indexed`, and nothing ever calls
  `attach_hd`. `to_sdl_surface` therefore expands it by nearest neighbour to
  132x132 texels at 4K. The palette swap between menu and game (palette 38 for
  red, 0 for yellow; `base_cursor_mode` L49-58 and `update_palette` L182-187)
  only recolours the indexed pixels.
* **Fix:** in HD, build an `HdImage` of the arrow in code at
  `22*k x 22*k`: rasterize the outline polygon with anti-aliasing, using
  colours 0x11/0x12/0x13 of the current cursor palette. `attach_hd` it to
  `cursor_surface_`, and rebuild it in `base_cursor_mode` / `update_palette`.
  Alternative: two pack keys, `plegacy/cursor.png` and
  `plegacy/cursor_menu.png`, loaded through `HdPack::vga`. The software path
  then draws it with no further change.

### H2. Fringe around sprites; the pins and corner pieces on paper pages. Effort S (engine or tool), M (redrawn art)

* **Where:** every BOB sprite. The worst cases are the paper pins and page
  corner pieces (`PageDecoration.cpp` `render_push_pin` L140-151 and
  `render_decorated_corners` L153-189; used by RestaurantStats, MarketResearch,
  StatsRestaurants, GlobalStats and WeeksTakings), night attackers and coins.
* **Why HD is lost:**
  * **AI packs.** `hdpack.py apply_reference_alpha` → `reference_mask` (L146-151)
    scales the original alpha with NEAREST and then applies a Gaussian blur
    (`--feather`, default 1.5; L818). The blur lowers alpha *inside* the
    outline as well as outside it. In the compositor, an opaque source pixel
    whose k x k block is not fully opaque gets its indexed pixel redrawn
    underneath (`needs_base`), so the 1994 staircase shows through the soft
    edge. Measured on pack6: 6.0% of all in-outline sprite texels have
    alpha < 240. By file: stanbobs 12%, stabo 13.6%, nightw* 12-16%,
    gaeste 10%, walk* about 7.5%, guest faces g* 2-9%. For the thin pins,
    nearly every in-outline block is affected.
  * **Validation does not catch it.** `hdpack.py validate` only flags texels
    with alpha < 64 (`hole_fraction` L267-274).
  * **MMPX packs.** Sprites are deliberately kept as magnified pixel art
    (`--method mmpx`; the 12x pin in `/tmp/pack12` is 12x12 flat blocks), so
    a sprite only 11-21 px across reads as blocky at 4K.
* **Fix:**
  1. *Engine, S:* in `Bob::frame` (and in `VgaLoader::load` for pictures with
     transparency), after `HdPack` returns the image, set alpha to 255 for
     every texel block whose indexed pixel is non-transparent (un-blend the
     colour), keeping soft alpha only on transparent pixels. Do this once per
     image at load, not per frame. Every existing pack is fixed, and
     `needs_base` becomes false inside sprites.
  2. *Tool, S:* feather outward only:
     `alpha = max(nearest_mask, blurred_mask)` in `reference_mask`. Also count
     alpha < 250 inside the outline in `validate`.
  3. *Art, M:* add stanbobs 26-40 (pins, corner pieces, arrows) and the coins
     and bars in lebbo and stabo to a redraw list like
     `furniture_redraw.json`, or paint them as vector art. MMPX cannot make
     11x14 sprites look high resolution.

### H3. Character portraits. Effort M

* **Where:** `src/views/base/RawRenderer.cpp` `RawRenderer::surface` L11-16,
  used by:
  * `views/helpers/CardGfx.cpp` `draw_framed_portrait` L16-36: the player
    turn card shown every turn (`PlayerTurnCard.cpp` L36-37), the staff hiring
    and staff cards (`StaffView.cpp` L886, L982, L1142) and the player profile
    (`PlayerProfile.cpp` L450);
  * `views/ui/widgets/CharacterPhotoBox.cpp` L11-20: character selection for a
    new game;
  * `views/ui/popups/AlertPopup.cpp` L59-64 and `ConfirmPopup.cpp` L55-58:
    manager, staff and opponent pictures (`TurnPresenterView`,
    `StaffEventPopups`, `RestaurantView` L1692).
* **Why HD is lost:** portraits are 52x59 pixels (palette 38) read from
  `chr/c%de.pc` (`CharacterFactory.cpp` L137-152). `AssetExporter` exports
  only `.vga` and `.bob` (`AssetExporter.cpp` L144-147), so a pack cannot
  contain them, and `RawRenderer` never calls `attach_hd`. A portrait is up to
  104x118 engine pixels, so on the turn card it is about 620x710 texels of
  blocks.
* **Fix:**
  * Export each portrait as `chr/c{N}e.png`, using `CharacterData` picture
    data and palette 38.
  * Add `HdPack::picture(key, size)`; the logic is the same as `vga()`.
  * Give `RawRenderable` a `virtual std::string_view hd_key() const`.
    `CharacterData` returns `chr/c{id}e`; job-market doubles return their
    `base_id`.
  * Have `RawRenderer::surface` attach the image. Every caller already uses
    `blit_raw`, which carries HD.
  * Portraits are a natural ComfyUI group (prompts.json: "Portraits... Good;
    check faces").

### H4. City view FAR zoom map (the default city view). Effort M

* **Where:** `src/views/screens/city_view/CityView.cpp`
  `render_map_zoom_far` L817-859. It fills one 2x2 rect per tile
  (`fill_rect` L852) into `far_map_cache_`, which is then copied with
  `blit_raw` (L857). The default is `zoom_(ZoomLevel::FAR)` (L99).
* **Why HD is lost:** the map is computed. A 160x120 tile grid becomes
  320x240 engine pixels of flat squares from a 256-entry colour table. There
  is no HD source, and the `blit_raw` clears the HD of the `back21` frame
  underneath. Each tile is a 12x12-texel square at 4K.
* **Fix:** build an HD far map once, next to `far_map_cache_`:
  * For each of the 256 tile types, area-average its HD `landsym` cell
    (32x32 engine px = 192x192 texels at k=6) down to 2k x 2k with
    `gfx::resample`, and cache the 256 thumbnails.
  * Compose them into a `320k x 240k` `HdImage` and `attach_hd` it to the
    cache surface.
  * Optionally blend the thumbnails toward the `palette_map` colour so the
    1994 colour coding stays readable, and tint for the season (see M4).
  * The highlight dots drawn on top stay flat, which is fine.
  * Fall back to the current flat map when `landsym` has no HD.

### H5. Text baked into pictures. Effort M (engine overlay) or S per picture (pack retouch)

* **Where (verified on the exports):**

  | Picture | Baked text | Drawn by |
  |---|---|---|
  | `gfx/back27` | "Advertising", "Flyers", "Posters", "Newspaper Ads", "TV-Ads" | `Advertising::render_overview` L150-151 |
  | `gfx/back21` | PLAYER, RANGE, LOCATIONS, PROPERTIES (the dropdown buttons; code draws only the opened lists, `DropdownList::render`) | `CityView::render_tabs` L1103 |
  | `gfx/back37` | "CAPACITY" | `AnimatedStats` L537 |
  | `gfx/back30` | "Pizza-Charts" | `MarketResearch` L739-740 |
  | `gfx/back31` | "NEWS ..." and fake text columns | `Newspaper.cpp` L66, `Advertising` L301 |
  | `gfx/back26` | "PIZZA" on the flyer | `Advertising::render_flyer` L272 |
  | `gfx/back36` | the cheque: "Pizzacheque", "SORT. Nr.", MICR digits | `ChequePopup.h` L40 |
  | `gfx/back28`, `gfx/blitmap5` | decorative pseudo-text (terminal) | `Terminal` |

* **Why it stays pixelated:** the letters are pixels in the 1994 picture.
  Upscalers keep them blocky, and diffusion models garble them
  (`docs/HD_REMASTER.md`: "Logos and anything with text").
* **Fix:**
  * *Engine:* a data file, e.g. `data/fonts/hd_labels.json`, listing per
    picture key the label rect (engine px), text, font face, colour and
    shadow. After `VgaLoader::load` attaches the pack image, draw the labels
    into a copy of the `HdImage` with `HdTypeface`, using the same placement
    as `prepare_hd_line`, so baked labels match the new HD text. The pack then
    supplies text-free (inpainted) versions of these pictures, or the engine
    erases the label rect from a clean sample of the same picture.
  * *Pack only:* retouch the listed PNGs by hand with a matching font. That is
    S per picture but does not follow the `hd_text` option.

---

## Medium visibility

### M1. Flat patches over textured HD backgrounds. Effort S

* **Where:**
  * `PurchasingMain::render` L31-34: a 208x88 rect filled with a colour
    sampled from the *indexed* `back3` at (165,80), to hide the sticker;
  * `StockMain::render` L54 (88x40 patch on `back3`);
  * `Newspaper::render` L69-71 (date box on `back31`);
  * `Advertising::render_purchasing_newspaper_ads` L302 and `render_viewing`
    L409 (`NEWSPAPER_SIDE_COVER`, a 100x284 patch on `back31`).
* **Why HD is lost:** `fill_rect` clears the HD picture there. In 1994 the
  patch matched the flat paper; in HD it is an untextured rectangle on
  AI-detailed paper. The sampled colour comes from the indexed picture, not
  from the HD one.
* **Fix:** patch with an HD-aware copy of a clean area of the same picture,
  e.g. `ctx.surface->blit(back3, ViewRect{clean area}, dst)` tiled over the
  rect, instead of `fill_rect`. Or provide pack variants without the
  sticker / date / column, e.g. key `gfx/back3.clean`.

### M2. City DISTRICTS and CONTROL tabs (hatched cells). Effort S-M

* **Where:** `CityView::draw_cell_with_pattern` L1158-1216 (`put` of 2x2
  dots, L1209), called from `render_with_color` L911-961 and
  `render_with_cell_color` L1004-1038. Results reach the screen through
  `render_districts` L963-989 and `render_control` L1040-1062 (`blit_raw`
  from a plain surface).
* **Why HD is lost:** the pattern is computed per pixel on a surface with no
  HD. The diagonal dot hatch becomes 12x12-texel dots at 4K.
* **Fix:** in HD, also build an `HdImage` for the viewport: flat cell colours
  plus thin anti-aliased diagonal lines with the same 8-pixel period and the
  same world anchor (`pattern_offset_x/y`), then `attach_hd` before the
  `blit_raw`. Drawing the hatch lines with the helper from M3 also works.

### M3. Diagonal strokes drawn as pixel stairs. Effort M (one helper; call sites unchanged)

* **Where:** `gfx::draw::line` (Bresenham with `put`, `Draw.cpp` L16-69),
  `line_2x` (2x2 blocks on the 320x200 grid, L71-104) and `fill_circle`
  (L175-185). Visible uses:
  * Market research economy graph, `MarketResearch::render_economy` L534-537
    (`line_2x`);
  * option checkboxes, `CheckboxMenuItem::draw` L45-55 (`line_2x` cross);
  * cookbook page chevrons, `CookBookPopup.cpp` `draw_chevron` L56-74;
  * tutorial popup chevron, `TutorialPopup.cpp` L100-101;
  * options slider knobs, `SliderMenuItem.cpp` L77-78 (`fill_circle`);
  * the diagonal edge in `GlobalStats.cpp` L1036.
* **Why HD is lost:** plain pixels. Horizontal and vertical lines are fine
  (flat); diagonals and circles are 6-texel stairs.
* **Fix:** in `gfx::draw`, when `hd_scale() > 1`, keep the indexed write and
  also rasterize the same stroke anti-aliased into a small `HdImage`, then
  blend it over with a transparent block. This is the same technique as
  `Font::finish_hd_line`. Apply it to non-axis-aligned `line` / `line_2x`
  and to `fill_circle`; the call sites stay as they are.

### M4. City map seasons and water in HD. Effort M

* **Where:** `CityView` constructor L177-189 (`palette_for_season`: 0, 0x43,
  0x44 or 0x45, plus RGB overrides for indices 28-31), `render` L681
  (`colors(season_palette_)`), and `update_water_palette` L1570-1579 with
  `setup_water_animation` L1581-1609 (indices 140-143 cycling).
* **Why HD is lost:** HD art has fixed colours from the export palette
  (landsym: 1, med1: 0). Measured:
  * landsym: 21% of pixels change colour in summer, autumn and winter;
    3.4% are water-cycle pixels;
  * med1: 22% of pixels use the season indices 28-31.

  So in HD the close and medium maps always look like spring with still
  water, while the indexed FAR map does change: zoom levels disagree.
* **Fix:** either:
  * pack variants per season (`gfx/landsym.summer.png` and so on), plus 4
    pre-tinted water frames swapped by `water_animation_.frame_index`; or
  * an engine tint at load: for each k x k block whose indexed pixel uses a
    remapped index, multiply its texels by `new_rgb / old_rgb`. The index is
    known from the indexed surface. Build 4 season images and 4 water images
    once, and switch which one is attached.

### M5. HD colours differ from the colours the screen shows. Effort M

* **Where:** the asset is exported with its metadata palette, but the screen
  displays it in another palette:
  * guest faces `gfx/g*` (export 0) in `GuestTalkPopup::render`
    (`surface.palette({4})` L229): 40% of face pixels differ;
  * capacity bars, lebbo 17-19 (export 4) on `back37` with palette 40
    (`AnimatedStats::render_capacity` L537-543): 18-54% differ;
  * `back3` (export 15) shown under palette 59 in `StockMain` L33 and
    palette 60 in `PurchasingMain` L29: 19% differ, mean delta 36-46.
  * `Bob::frame(i, palette)` overrides (cars, door, menu arrows, chopper
    blades) only matter when the *layer* ends up in that palette. They were
    checked and do not differ here.
* **Why:** the colours in a pack picture are absolute
  (`docs/HD_REMASTER.md`), but the same indices are shown in a different
  palette on these screens.
* **Fix:** the export records each (key, display palette) pair the game
  uses. The pack gets variants with a palette suffix (`gfx/g0.p4.png`), and
  `VgaLoader` / `Bob::frame` take an optional "displayed in" palette to pick
  the variant. Cheaper engine route: the per-block ratio tint from M4.

### M6. Medium zoom map and restaurant overview thumbnails. Effort M (shares code with H4)

* **Where:** `CityView::render_map_zoom_medium` L789-815 and
  `StatsRestaurants::draw_restaurant_overview` L132-147 (the 8x8-tile map in
  global statistics → restaurants).
* **Why limited:** the path is HD-aware (`med1` blits), but each med1 tile is
  4x4 original pixels, so at 12x any pack can only give smudge or pixel art.
* **Fix:** derive the medium tiles from HD `landsym`, area-averaged 4:1
  (192 → 48 texels per tile at k=6), and attach them to `medium_map_cache_`.
  The thumbnails reuse the same cache.

### M7. Motion and scrolling snap to the 640x400 grid. Effort L

* **Where:** `GuestWalkScene::drawn_position` L463-480 (positions are rounded
  to engine pixels), sprites drawn with `blit_masked`, and the camera offsets
  in the restaurant and city views.
* **Why:** HD commands are placed at `engine px * k`, so smooth motion and
  scrolling move in 6-texel steps at 4K.
* **Fix:** let a sprite blit carry a sub-pixel texel offset (`HdCmd.dx/dy` are
  already in texels). The indexed copy and floor bookkeeping stay at the
  rounded position; only the HD command is shifted, by the fractional part of
  the lerp or camera position. This needs care where cmds are clipped in
  `hd_transfer`.

---

## Low visibility

### L1. Stat bars (staff cards, pizza info). Effort S

`StatBar::create_stat_bar` L72-75 runs
`replace_colors({c1, c2}, TRANSPARENT)` over the whole composed bar, and
`Surface::replace_colors` clears HD on every changed pixel. The corner fix
punches 6x6-texel holes wherever those two colours occur in the HD bar.
**Fix:** compute the mask once from the indexed `bar_empty_` corners. In HD,
skip `replace_colors` and rely on the pack's alpha, or zero only the texels of
the corner pixels.

### L2. Half-size pizza (recipe preview, cookbook). Effort S

`pizza_design::draw_half_size_pizza` L36-45 masks the outside of a circle with
`put`. The HD pizza (correctly averaged by `blit_half`) gets a stair-stepped
rim. **Fix:** in HD, blend an anti-aliased ring of `outside_color` as an HD
picture (a coverage mask at `k` resolution), as in M3.

### L3. Busy cursor (padlock, at intro start and during loading). Effort S

`views/ui/BusyCursor.cpp` `icon_surface` L20-29 blits from `gfx/icons` with
the colour key `ICON_BACKGROUND` (249). `Surface::blit` / `hd_transfer`
(L132-231, L407-411) treat only the *indexed* key as transparent; the HD
command still covers the whole rect, and the HD icons sheet has the
background opaque. In HD the padlock shows a coloured square. **Fix:** when
`transparent != 0` and the source has HD, materialize the rect and zero the
alpha where the source pixel equals the key before pushing the command.

### L4. Hardware cursor mode (`software_cursor = false`). Effort S

`CursorManager::hw_apply_cursor` L189-223 converts with
`Renderer::to_sdl_surface(source_surface)` (scale 1, L196), which ignores HD,
then scales with NEAREST. Topping cursors and the busy cursor lose their HD
art. **Fix:** pass `scale = hd_scale()` and scale down to the window scale
with LINEAR.

### L5. Capacity screen bars. Effort S

`AnimatedStats::render_capacity`:
* L572-577 and L599-605 build the bars from repeated 2-px slices of the HD
  sprite. If the AI art is not uniform there, the slices show seams.
* L631-640 paint the red fill as 18 flat 2-px `fill_rect` rows over the HD
  bar, which shows as hard bands.

**Fix:** stretch one slice as an HD picture (an `HdCmd` with `sw` < `dw`, or
`resample`), and draw the gradient as an HD vertical gradient.

### L6. First-run game-location screen. Effort S

`GameLocationScreen.cpp` L70 loads the engine's own
`data/game_location.vga`, key `plegacy/game_location`. The exporter only
walks the game folder, so this key never gets an HD file. **Fix:** ship
`plegacy/game_location.png` with the release or with `assets/hd`.

### L7. Atlases upscaled as one image (pack tooling). Effort M; not checked visually

`hdpack.py` treats sprite and tile atlases as one picture: `landsym`, `med1`,
`icons`, `blitmap1-5`, `k0-k13`, `spot*`, `tvmisc`, `orden`, `permap`. AI
upscalers mix neighbouring cells, which is likely to show as seams where
`landsym` tiles repeat at close zoom and as bleed at icon edges. The
`Blitmap` rects add one extra engine column (`Blitmap.inl`, `x2*2+1`), which
can show a sliver of the neighbouring HD icon. **Fix:** split atlases into
cells (known grids; the `gfx/offsets*` files for blitmaps), upscale each cell
with repeated-edge padding, and reassemble.

### L8. Exports with an unknown palette. Effort S (to verify)

The export manifest has `palette_known: false` for `bob/typ10e`, `bob/walk0`
and `bob/walk10`, which were exported with palette 0. Their HD colours may be
wrong. Check them in game and fix the entries in `AssetMetadata.cpp`.

(The debug viewers under `src/views/viewers` use palette rotation and raw
frames by design; they are not part of the game.)

---

## Checked and HD-correct (no action)

* **VGA backgrounds** through `VgaManager` with `blit`, `blit_raw`,
  `blit_with_palette` and `blit_raw_with_palette`: menus, rooms, Euromap,
  newspaper images `z/zb*`, jury, maestro, ending, `zw*` full-screen messages,
  posters `plakat*`.
* **The intro** (`Intro::render`): `mplogolo`, `titel1`, `titel2`, `cyber`,
  plus code-drawn credit text. Its only defect is the busy cursor (L3).
* **TV commercials** (`TVPlaybackEngine::next_frame`): frames are blitted
  from the HD `tvmisc` / `spot*` sheets, and the title card is HD text.
  `Advertising::render_tv` uses `blit_raw`, which carries HD.
* **Blitmap icons** (`Blitmap` is a clone of the VGA; `render_icon` and
  `icon_surface` use `blit`), **buttons** (`Button::make_button_surface` from
  `permap` + `icons`), cork and binder decorations, demographic icons and
  `PIXEL_LINE` bars, orden medals, StaffView cards built with
  `SurfaceBuilder` from `permap`.
* **BOB sprites:** furniture, ovens, guests (seated and walking), actors
  `typNe` (with the eye-blink overlay), toppings, chopper blades, attack and
  weapon sprites, stats axes and bars, coins. All go through `Bob::frame`
  and `blit_masked`; the only caveats are H2 and M5.
* **Half-size pizza** (`blit_half` averages HD texels; rim aside, L2), card
  slider, popups, and all flat UI (boxes, bevels, grids, horizontal and
  vertical lines, flat bars, highlight dots, judges' score cards).
* **All code text:** every path ends in `Font::render_line_onto`, which in HD
  writes only the HD letters (`blit_stencil` runs only when HD text is off).
  The typewriter, tooltips and status panel all use it.
* **Custom cursors in software mode** (the default): the topping and
  zoom-box cursors keep their HD art.
* **The lighting pass** (`LightPass`) uses LINEAR textures, so it adds no
  blockiness.

## Notes and uncertainties

* Which pack produced the user's 4K screenshots is unknown. The pin analysis
  (H2) uses the local packs: the pack6 AI art has feathered alpha, and the
  `/tmp/pack12` MMPX art is hard-edged. Either way the code path is HD-aware;
  the fixes are the alpha normalization in the engine or tool, and redrawn
  art for small UI sprites.
* The code-drawn labels in symptom 3 (MAP/DISTRICTS/CONTROL, terminal header)
  should already be HD text at HEAD. Someone is editing
  `src/resources/HdTypeface.*` in this checkout; re-shoot after that lands.
* L7 (atlas bleed) and the visual size of M7 were inferred from code and
  tooling, not seen.
* Palette numbers in M4 and M5 come from mapping the exported RGB back to
  indices with the export palette, then comparing with `GFX/PALETTE.E`. A
  difference counts if it is more than 30 in summed RGB.
* Optional stopgap for anything still without HD (H3, H4, M2 before they are
  fixed): run MMPX over each layer's indexed base in
  `HdCompositor::upload_base` at 2x, then draw it LINEAR. This smooths
  diagonals at a small CPU cost. It is not a substitute for real HD sources.
