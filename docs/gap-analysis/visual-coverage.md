# Visual coverage: what still stands between Pizza Tycoon HD and an end-to-end HD remaster

Static, read-only cross-reference of every screen, popup and asset family against the HD
pipeline. Working tree of 2026-11-14 (after 94d616e "HD text"). Nothing was built or run.

Inputs: `docs/blueprint/screens.md` (screen list), `docs/blueprint/file-formats.md` and
`content-data.md` (asset families), `docs/hd-audit.md` (earlier audit, H1-H5, M1-M7, L1-L8),
`docs/HD_REMASTER.md`, `docs/CHANGELOG.md`, the HD code (`src/resources/HdPack.*`,
`src/engine/gfx/Surface.cpp`, `Draw.cpp`, `HdList.*`, `src/engine/HdCompositor.cpp`,
`src/resources/Font.cpp`, `HdTypeface.*`, `src/engine/ui/CursorManager.*`,
`src/engine/LightPass.cpp`, `src/views/**`), the pack tools (`tools/hdpack/*`,
`data/fonts/hd_fonts.json`), a file listing of the local game folder, and the local export and
packs (`/home/claude/w/export`, `pack6`, `/tmp/pack12`). The exports were looked at only to
find out which pictures carry lettering. No original art or text is reproduced here; assets
are named by file only.

**Moving target.** `Font.cpp`, `Surface.cpp`, `HdCompositor.cpp` and `HdList.h` were being
edited while this was written (HD text is moving to tinted glyph quads: `HdCmd::tint`,
`Surface::hd_overlay`). Line numbers in those four files are as of 11:14 local time; the
function names are the stable reference.

Effort: **S** under half a day, **M** 1-3 days, **L** more than 3 days. Risk: chance of
visible regressions or of breaking the "game logic never sees HD" rule.

---

## 0. Summary

* The pipeline makes HD available for exactly three sources: VGA pictures
  (`VgaLoader::load`, VgaLoader.cpp L133-143), BOB frames (`Bob::frame`, Bob.cpp L61-69) and
  code text (`Font::render_line_onto` → `HdTypeface`). Everything else (cursor, portraits,
  computed maps, primitives, palette effects, the engine's own pictures) is drawn at
  640x400 and magnified by nearest neighbour (`HdCompositor::upload_base`, L454-466).
* Of the 51 screen/popup rows in §2.1-2.5, **none is fully HD**: the mouse arrow (H1) is on
  every one. Ignoring the cursor, 11 rows are fully HD (flat code chrome counted as HD), 29
  are partial, and 11 have at least one non-HD column: game location, characters, city view,
  market research, staff, animated statistics, turn card, alert/confirm, cook book, guest
  talk, tutorial popup.
* All 20 hd-audit findings (H1-H5, M1-M7, L1-L8) are confirmed (a few line numbers drifted;
  §5). This audit adds **21 new findings (N1-N21)**: the newspaper's whole article area is a
  flat patch, the
  padlock bug shows on every turn, the cursor snaps to the 640x400 grid, more baked text
  (posters, TV banners, mafia shirt, city signage, logos), mislabelled AI prompts, more
  palette-swap pairs, silent bilinear magnification of low-res packs, and no gate that
  measures HD coverage.
* The shipped release contains one HD picture (`assets/hd/gfx/titel1.png`). Everything else
  depends on a pack the user builds; local packs cover 100% of the 1,699 exportable keys but
  nothing outside `.vga`/`.bob`.

---

## 1. How HD reaches the screen, and every way it is lost

| # | Mechanism | Code | Effect at hd_scale k |
|---|---|---|---|
| S1 | VGA picture + pack PNG | `VgaLoader::load` L133-143 → `Surface::attach_hd` (Surface.cpp L80-90) | HD |
| S2 | BOB frame + pack PNG | `Bob::frame` L61-69 | HD |
| S3 | Code text | `Font::render_line_onto` (Font.cpp L607+), HD branch L657-660, `prepare_hd_line` L226+ | HD letters only; indexed pixels get none |
| C1 | Carriers that keep HD | `blit` L396-436, `blit_raw` L306-352, `blit_masked` L523, `blit_half` L457-521, `clone` L70-76 | HD moves with the pixels |
| D1 | Raw bytes, never `attach_hd` | `Surface::from_indexed` L33-68 from CursorManager.cpp L38-42, RawRenderer.cpp L11-16 | k x k blocks |
| D2 | Computed per pixel/tile | `CityView::render_map_zoom_far` L817-859, `draw_cell_with_pattern` L1158-1216, HalfSizePizza.cpp L37-45 | k x k blocks |
| D3 | Non-axis primitives | `draw::line` (Draw.cpp L35-67), `line_2x` L70-105, `fill_circle` L175-185 | k-texel stairs |
| D4 | Plain overdraw clears HD | `put` L268-277, `fill_rect` L287-304, `replace_colors` L533-569, blit from a non-HD source L415-428 | flat colour hole in HD art |
| D5 | `fill` resets the list | `Surface::fill` L279-285 | whole surface flat |
| D6 | Colour key ignored on the HD side | `Surface::blit` with `transparent != 0` → `hd_transfer` L156-260 | opaque HD rect |
| D7 | Soft alpha inside the outline | `HdCompositor::draw_layer` L507-590, `needs_base` L536-541; HdList.h L31-41 | 1994 staircase shows through |
| D8 | Palette effects | layer palette chosen at upload (`upload_base` L454-490); HD texels fixed | HD keeps export colours |
| D9 | Integer engine positions | `HdCmd.dx/dy = engine px * k`; PointerTracker.cpp L23-28 | motion in k-texel steps |
| D10 | Pack content | MMPX pixel art, AI garbling, atlas bleed, feathered alpha, silent resample (HdPack.cpp L185-193) | soft or blocky "HD" |

Fades are HD-correct everywhere: `draw_layer` multiplies every HD command by the fade factor
(HdCompositor.cpp L570-578) and the CPU path does the same (`Renderer::to_sdl_surface`
L585-616).

---

## 2. Coverage matrix (one row per screen/popup in `screens.md`)

Legend: **Y** HD, **P** partial, **N** not HD, **–** not applicable. Columns: BG = background
pictures; Sprites = BOB frames, atlas cells and pictures on top; Text = code text (HD
typeface) vs lettering inside pictures; Cursor = the arrow is **N** on every screen (H1) and
moves on the 640x400 grid (N3), so the cell only names screen-specific cursors; Prims = rects,
lines, hatching, circles (flat axis-aligned = Y, they are crisp at any k); Anim = frames and
motion; Palette = fades (always Y) plus swaps, cycling, seasons. Gap IDs refer to §5 and §6.

### 2.1 Start-up and menus

| Screen | BG | Sprites | Text | Cursor | Prims | Anim | Palette | Known issues |
|---|---|---|---|---|---|---|---|---|
| Game location | N engine picture, no pack file (`GameLocationScreen` ctor L70, `render` L148) | – | N lettering baked in `data/game_location.vga`; live path L153-172 needs `.fnt` files that are not there yet | N | Y `draw::box` L130, L183 | – | – | L6, N21 |
| Intro | Y `Intro::render` L148-171 (mplogolo, titel1 shipped 4K, titel2, cyber) | – | Y credits/version L156-191; P logo lettering in titel2, cyber, mplogolo (N6) | N + padlock P (L3) | Y flat fills | Y logo cuts; fades Y | Y | H1, L3, N6 |
| Main menu | Y `MenuMain::background` L80-82 | Y `ArrowMenuItem` L16-17 (menubobs 3/4, palette 63) | Y; P logo lettering in gfx/menu (N6) | N (red arrow, `CursorManager::base_cursor_mode` L49-58) | Y | – | Y | H1, N6 |
| New game, Quick start, Difficulty, Disk menu, Save, High scores | P flat fill + menubobs frame 1 (`Menu::default_background` L482-507) | Y | Y (title L504) | N | Y help rule L452, title rule L497-502 | – | Y | N11 |
| Options, Key bindings | P as above | Y | Y | N | P checkbox cross `line_2x` (`CheckboxMenuItem::draw` L49-54), slider knob `fill_circle` (`SliderMenuItem` L77-78) | – | Y | M3, N11 |
| Mission select, Load game, Save as game, Save as mission, Recipe export | P default bg + flat boxes (`MenuGameLoad::background` L227-240 and the same in the others) | – | Y | N | Y flat list widgets (`TextListBox` L83-97, `SaveFileListBox` L41-62, `SaveFileInputBox` L35-88) | – | Y | N11 |
| Recipe import | P as above | Y zutaten (`RecipePreview`) | Y | N | P half-size pizza rim (`RecipePreview` L39 → HalfSizePizza.cpp L37-45) | – | Y | L2, N11 |
| Characters | P flat boxes (`CharacterSelection::background` L154-178) | N portraits (`CharacterPhotoBox` L11-20 → `RawRenderer::surface`) | Y | N | Y (`CompanySelectionBox` L46-79) | – | P portraits are palette 38 | H3, N11 |

### 2.2 World screens

| Screen | BG | Sprites | Text | Cursor | Prims | Anim | Palette | Known issues |
|---|---|---|---|---|---|---|---|---|
| Euro map (all modes) | Y `EuroMap::render` L120 | – | Y | N | Y flat bevel boxes (`beveled_box` L257) | – | Y palette 2 = export | N11 |
| City view | P frame back21 Y (`render_tabs` L1103); FAR map **N** (`render_map_zoom_far` L817-859, default zoom L99); MEDIUM P (`render_map_zoom_medium` L789-815); CLOSE Y with atlas seams (`render_map_zoom_close` L722-787) | Y cars (`CarManager::render` L90-120); highlight squares flat (`render_highlights` L1384-1445) | P tabs Y (L1127); dropdown labels baked in back21 (H5); shop/bank signage baked in landsym (N6) | N; zoom box Y flat (`create_zoom_box_cursor` L881-895) | **N** DISTRICTS/CONTROL hatching (`draw_cell_with_pattern` L1158-1216) | P cars and scrolling snap (M7); water cycling N (M4) | **N** season palette (ctor L177-189, `render` L681) and water cycle (`update_water_palette` L1570-1579) | H4, M2, M4, M6, H5, N6, L7, N18 |
| Player profile | Y back32-35 (`render` L217) | P orden, stanbobs arrows (L347-353) Y; portrait N (L450 → `draw_framed_portrait`) | Y | N | Y flat bars L526, L611, L627 | P card slides in engine px (`CardSlider::render` L98-101) | Y | H3, H2, M7 |
| Market research | P back30 Y (L739); other pages code paper (`render` L188-190) and cork from permap (`render_economy` L499-501) | P pins/corners (H2); landsym icons on flat squares (L146-148) | Y; P chart title baked in back30 (H5) | N | N economy graph `line_2x` L534-537; grid Y | – | P palette 62 / 8 / 7 switched mid-render (L47, L188, L499) | H2, H5, M3, N5, N10 |
| Newspaper | P back31 Y but flat patches: date box L69-71, article surfaces L91-96 → L132-135, side column L118-121 | Y z/zb picture (L122-124) | Y; P masthead lettering baked in back31 (H5) | N | P patches | – | Y | M1, **N1**, H5 |
| Advertising (overview, flyers, posters, newspaper ads, TV) | Y back24-27, back31 | Y posters (`render_poster` L291-297), TV frames (`render_tv` L426-429) | Y flyer/ad text (L273-283, L364-383); P labels baked in back27 and back26 (H5); poster slogans, VCR digits in back24, TV banner lettering in tvmisc (N6) | N | P flat patches: selected channel L179-184, newspaper side cover L302, L409, hover L317 | P TV frames from sheets at the original rate (`TVPlaybackEngine::next_frame` L84-128); sheet bleed (L7) | Y | H5, N6, M1, N19, L7 |
| Company statistics (0, 2-7) | P code paper (`GlobalStats::render` L62-64, `render_background` L182-199); binder Y (L142-180) | P stabo, stanbobs (H2) | Y | N | P one diagonal (`render_balance_lines` L1036); other lines axis-aligned | – | P palette 8 and 43 (L62, L482) with stabo (42), blitmap2 (7) | H2, M3, N5, N10 |
| Restaurants overview (1) | P code paper | P med1 thumbnails capped at 4x4-px tiles (`draw_restaurant_overview` L130-146) | Y | N | Y lines L196-232 | – | P | M6, H2, N10 |
| Terminal | Y back28 (`render` L382-383), blitmap5 (L85) | – | Y; P decorative pseudo-text in back28/blitmap5 (H5) | N | Y highlight box L322 | – | Y | H5 |
| Actor conversation (all actors, supplier contract) | Y back{N} (`ActorView::render` L274-277) | P typNe heads Y; eye-blink cell cut from the same sheet (L286-300, N8); typ10e palette unknown (L8) | Y; P slogan baked in back20 (N6) | N | Y contract box L334 | P nod and blink in engine px (L289-315) | P head sheets (palette 22) shown in the back{N} palette (N5) | N5, N8, L8, N6 |
| Mafia mission | Y back38 (L54) | – | Y | N | – | – | Y | – |
| Depot main, weapons, export, stock, supplier list, order | Y back3 / back14 / back9 (`DepotMain` L43-45 palette 15 = export) | Y permap, blitmap | Y | N | P flat text box (`DepotWeaponInterface` L39, L132), opaque hover bands (`DepotExportScreen` L126, `DepotIngredientOrderScreen` L210, `DepotWeaponInterface` L68) | – | Y | N11 |

### 2.3 Restaurant screens

| Screen | BG | Sprites | Text | Cursor | Prims | Anim | Palette | Known issues |
|---|---|---|---|---|---|---|---|---|
| Restaurant (dining, kitchen, all `RestaurantViewMode`s) | Y floors/walls from k0 and k{N} (`RestaurantView` L888-917, L1320-1425), doors L1437-1450; tile seams (L7) | Y furniture (286 frames AI-redrawn), guests, walkers, staff, pizza; fringe (H2); walk0/walk10 palette (L8) | Y (weapon list L1592) | N + furniture cursor Y (`FurniturePlacementController` L492, L527, L760) but snaps (N3) | Y hover box L1586 | P original frame counts; smooth-motion lerp rounded (`GuestWalkScene::drawn_position` L463-480); camera snaps | Y lighting is HD-native (`LightPass::apply`); layer palette 4 (L1256) with gaeste (16) (N5) | H2, M7, L7, L8, N5, N3 |
| Night attack | Y | Y nightp/nightw (`AttackScene` L541-580, L1174-1220); 12-16% soft texels (H2) | – | N | – | P engine-px motion | Y | H2, M7 |
| Menu (prices) | Y back2/back9 | – | Y | N | P opaque selection/edit bands L198, L241 | – | Y | N11 |
| Stock | P back3 + patch (`StockMain::render` L54) | – | Y | N | P patch | – | P palette 59 after back3 (L33) | M1, M5 |
| Stock category | Y back9 | – | Y | N | Y rules (`IngredientCategoryView` L94) | – | Y | – |
| Supplier list | Y back9 + blitmap1 banners (`SupplierListRenderer.h` L65-74) | Y | Y | N | – | – | Y | L7 |
| Ingredient order | Y back9 | – | Y | N | P opaque hover band L213; rules Y L192-198 | – | Y | N11 |
| Contracts | P back5 + flat stacked-page patches (`Contracts` L222-235) | – | Y | N | P patches; separator L153 | – | Y | N19 |
| Purchasing | P back3 + sticker patch with a colour sampled from the indexed picture (`PurchasingMain::render` L29-34) | – | Y | N | P patch | – | P palette 60 (L29) | M1, M5 |
| Category overview | Y back9 (L83), floor preview from k{N} (L551-554, L231) | Y | Y | N | P flat panels L641-675 | – | Y | N11, L7 |
| Supplier list (equipment) | Y | Y | Y | N | – | – | Y | – |
| Furniture list | Y back9 | Y moebel/kuebobs | Y | N | P flat box behind each item (`FurnitureListView` L188-195) | – | Y | N11 |
| Floor list | Y back9 | Y floor previews (`FloorListView` L140) | Y | N | – | – | Y | L7 |
| Staff (overview, applications, card, training) | P built from permap slices on a flat fill (`StaffView::render` L179-215) | N portraits (L886, L982, L1142); P stat bars (`StatBar::create_stat_bar` L72-75) | Y | N | Y boxes L646-651, rule L727 | P card slides | P palette 13 with permap (0) | H3, L1, N20, N5, N10 |
| Restaurant statistics (0-2, 4, 5) | P code paper (`RestaurantStats::render` L40-44, `render_background` L163-180, `render_paper` L344-347); binder Y | P pins/corners (H2); stabo bars stacked every 2 px (L917-929) | Y | N | Y axis lines L187-191, L399-421, L539-541 | – | P palette 8 / 7 (L41, L826) with blitmap2 (7) and stabo (42) | H2, N5, N10, N12 |
| Week's takings (3) | P code paper (`WeeksTakings::render_background` L87-99) | P corners (H2) | Y | N | Y L316-318 | – | P | H2, N10 |
| Animated statistics | P back37/back6/back9 Y; reputation/quality/turnover on code paper (`draw_decorated_background` L256-265) | P lebbo, blitmap4, stanbobs; capacity bars with palette 40 (L541-543); bills `blit_raw` L952 | Y; P label baked in back37 (H5) | N | **N** capacity red bands (L636-640), repeated 2-px slices (L574-583, L603-607) | P strip scroll in engine px (L328-349), thermometer, bills | **N** palette 7 after back9 (L712-714), 42 (L783), 40 (L541) | H5, L5, M5, N5, N10, N12 |
| Pizza designer (recipes, view, ingredients, design, info, competition) | Y back1/back4 (`PizzaDesignCanvas` L205, `PizzaDesignInfo` L41, L127, `PizzaDesignRecipes` L38, `PizzaDesignCompetition` L232, L250) | Y toppings, chopper blades (`PizzaDesign.cpp` L65-81) | Y | N + topping cursor Y (`PizzaDesignCanvas` L132-335) but snaps (N3) | P stat bars (L1), half-size pizza (L2); rules Y (`PizzaDesignInfo` L144-148) | P blade swap, drag snap | P Info mode custom palette 18 + ramp from 13 (`PizzaDesign.cpp` L344-347) | L1, L2, N3, N5 |
| Judges | Y jury (`JudgeAnimator` L65) | – | Y | N | Y flat score cards (L114-128) | P card raise in engine px | Y | N11 |

### 2.4 Turn and event screens

| Screen | BG | Sprites | Text | Cursor | Prims | Anim | Palette | Known issues |
|---|---|---|---|---|---|---|---|---|
| Player turn card | Y back39 (`PlayerTurnCard::render` L23) | N portrait (L36 → `CardGfx::draw_framed_portrait` L16-36) | Y | N + padlock P while the pipeline runs (N2) | Y flat frame (`CardGfx` L28) | – | Y | H3, N2 |
| Full-screen milestone | Y zw{N} (`FullscreenStatus` L62, L112) | – | Y | N | – | – | Y | – |
| El Maestro message | Y maestro (L46) on flat fill (L44) | – | Y | N | – | – | Y palette 41 = export | – |
| Game over | Y maestro (L152-154) | – | Y | N | – | – | Y | – |
| Win sequence | Y ende (L71) | – | Y | N | – | – | Y | – |

### 2.5 Popups

| Popup | BG | Sprites | Text | Cursor | Prims | Anim | Palette | Known issues |
|---|---|---|---|---|---|---|---|---|
| AlertPopup, ConfirmPopup | – flat chrome (`BasePopup::popup_surface` L71-99) | N portraits (AlertPopup L59-64, ConfirmPopup L57-65) | P HD letters coloured from palette 0, popup shown in palette 38 in menus (N4) | N | Y flat bevels | – | P | H3, N4, N11 |
| OptionsPopup, NumberInputPopup, TextInputPopup, ContextMenuPopup, InvoicePopup, competition popups, turn invoice popups | – flat chrome (`BasePopup` L52-99, `ContextMenuPopup` L27-29, `FinanceComponents` L36-43, `TextInputPopup` L46, `NumberInputPopup` L66) | Y stanbobs arrows (number input) | P (N4 in menu mode) | N | Y | – | P | N4, N11 |
| ChequePopup | Y back36 (L20-21) | – | Y; P cheque lettering baked (H5) | N | – | – | Y | H5 |
| CookBookPopup | Y back1 | Y zutaten | Y | N | N chevrons `line_2x` (`draw_chevron` L56-74); half-size pizza (L150) | – | Y | M3, L2 |
| GuestTalkPopup | – flat frame (L192-200) | P face sheet g{N} (L203-209) + eye/mouth cells from the same sheet (L211-222) | Y | N | Y | P overlay cells (N8) | N faces exported in palette 0, shown in 4 (L229) | M5, N8 |
| JukeboxPopup, TvPopup | – code-drawn (JukeboxPopup L78-101, TvPopup L62-85) | – | Y | N | Y | – | Y | N11 |
| TutorialPopup | – flat (L82-84) | – | Y | N | N chevron diagonals (L100-101) | – | Y | M3, N11 |

### 2.6 Layers and shared UI

| Element | State | Evidence | Known issues |
|---|---|---|---|
| TooltipLayer | Y text, flat box | `TooltipLayer.h` L63-69 | N11 |
| TurnLockLayer | P padlock cursor is an opaque square | TurnLockLayer.cpp L20, L41-42 → `icon_surface` (BusyCursor.cpp L20-29) | N2 (L3) |
| TutorialLayer | Y | box L425, text L351 | – |
| GameInfoLayer, PauseLayer | Y text | `draw_screen_text` (ScreenText.cpp L24-32) | – |
| ConsoleLayer, DebugLayer | developer only (flat) | ConsoleLayer.cpp L31-43, L111-133 | – |
| Side panel frame (gfx/bo) | Y | `PanelContainerView.h` | – |
| Buttons (permap plates + icons) | Y; edges may show neighbour cells (L7) | `Button::make_button_surface` L24-78 | L7 |
| Menus (menubobs) | Y | Menu.cpp L483-488, `ArrowMenuItem` | – |
| Page decorations, binders, bars (blitmap1-4) | Y; atlas bleed, soft alpha | `PageDecoration.cpp` L28-190, `Blitmap.inl` L49-58, L89-95 | L7, H2 |
| Status panel | Y text; flat energy bars | `StatusDisplay` L229-235 | – |
| Developer viewers (`src/views/viewers`) | out of scope | – | – |

---

## 3. Per-asset-family table

"Replaceable" = the HD pack has a key for it. "Done so far" = what the repository ships plus
what the local packs contain (pack6: SDXL refine, 6x the original; `/tmp/pack12`: MMPX 12x;
both 125 gfx + 20 z + 1,554 BOB files, i.e. every exportable key).

| Family (files) | How it reaches the screen | Replaceable | Done so far | Gap |
|---|---|---|---|---|
| Room/page backgrounds `gfx/back1..back40` (37 files) | `VgaManager::get` → S1 → `blit_raw_with_palette` / `blit_masked` | Y `gfx/backN.png` | Local packs only; one prompt rule calls all of them "empty restaurant room" (prompts.json L35-39) | N7 prompt; H5/N6 lettering; M1/N1/N19 patches; M5/N5 palette; back8 and back40 unused by code |
| Full-screen pictures `menu`, `titel1/2`, `cyber`, `mplogolo`, `euromap`, `jury`, `maestro`, `ende`, `zw1..11`, `bo` | S1 | Y | `titel1` shipped as 4K art; rest local packs | Logos need hand-made redraws (N6); zw/ende/bo have no prompt rule (default 0.35) |
| Atlases `landsym`, `med1`, `icons`, `blitmap1..5`, `permap`, `k0..k13`, `orden`, `tvmisc`, `spot0..8` | S1 + sub-rect `blit` (`Blitmap::render_icon_internal` L89-95, `landsym_source_rect`) | Y, whole sheet only | Upscaled as one picture | L7 bleed; k* prompted as "UI icons" though they are floors and walls, blitmap* as "room detail", permap as "map" (N7) |
| Guest faces `gfx/g0..g23` | S1, cells cut for face/eyes/mouth | Y | Local packs | M5 palette 0 vs 4; N8 overlay cells |
| Posters `gfx/plakat0..8` | `Advertising::render_poster` L291-297 | Y | Local packs | Slogan lettering (N6); prompt denoise 0.3 garbles it |
| Newspaper `gfx/back31` + `z/zb1..20` | `Newspaper::render` L64-135 | Y | Local packs | N1, M1 patches; masthead lettering (H5) |
| TV ads `gfx/spot0..8`, `gfx/tvmisc` | `TVPlaybackEngine::next_frame` L84-128 → `Advertising::render_tv` L426-429 | Y (sheets) | Sheets upscaled whole | L7 frame bleed; banner lettering in tvmisc (N6); original frame rate (3 ticks/frame, L63-77) |
| BOB sprite sets: furniture `moebel` (341), kitchen `kuebobs`, toppings `zutaten` (322), guests `gaeste`, walkers `walk*`, actors `typ*e` (25x6), night `nightp/nightw*`, UI `stanbobs`, `stabo`, `lebbo`, `menubobs`, `stas`, `kus`, `mens` (51 files, 1,554 frames) | S2 → `blit_masked` | Y `bob/<file>/NNNN.png` | MMPX for all (pack12); SDXL for all (pack6); 286 furniture frames repainted with depth ControlNet (`furniture_redraw.json`: 275 moebel + 11 kuebobs; art not in repo) | H2 alpha fringe; MMPX stays pixel art; animation frames drift (N8, HD_REMASTER.md L204); L8 palettes; tiny UI sprites (pins, corners, arrows, coins) need vector redraws |
| Fonts `fonts/*.fnt` (6 + MINI = SUPERSMA at scale 1) | `Font::render_line_onto` → `HdTypeface` (FontLoader.cpp L199-203, `hd_fonts.json`) | Not via pack; 7 TTF mappings | Done (OFL fonts) | N4 colour from the drawing palette; missing glyphs become the 1994 substitute or a space silently (`hd_letters` L377-395); over-wide lines are squeezed |
| Cursors: arrow (array in the executable), padlock (icons cell), zoom box (code), furniture/topping (BOB frames) | `CursorManager` L21-42; `icon_surface` L20-29; `create_zoom_box_cursor` L881-895 | Arrow: no key; padlock via `gfx/icons` | None for the arrow | H1, N3, L3/N2, L4 |
| Portraits `chr/c0e..c99e.pc` (59x52, palette 38; doubles reuse them) | `CharacterFactory` L137-152 → `RawRenderer::surface` L11-16 | **N** (no key, not exported: AssetExporter.cpp L144-147) | None | H3, N13 |
| City map, CLOSE zoom | landsym 16x16 tiles x2 (`render_map_zoom_close` L722-787) | Y (landsym) | Atlas upscaled whole | L7 seams, visible 32-px tile repetition at 4K, signage lettering (N6, N18), season/water (M4) |
| City map, MEDIUM zoom | med1 4x4 tiles (`render_map_zoom_medium` L789-815) | Y (med1) but capped | med1 upscaled | M6 |
| City map, FAR zoom (default) | one flat 2x2 rect per tile (`render_map_zoom_far` L817-859) | **N** | – | H4 |
| City DISTRICTS / CONTROL tabs | per-pixel hatching (`draw_cell_with_pattern` L1158-1216) | **N** | – | M2 |
| Euro map `gfx/euromap` | `EuroMap::render` L120 | Y | Local packs | – |
| Intro and logos | `Intro::render` L144-198 | Y | titel1 only | N6 |
| Minigames: pizza design (back1/back4, zutaten, chopper), competition (jury, judges), newspaper-ad builder (back31 + code text), TV/jukebox pickers (code) | as the rows in §2 | Y pictures; code parts N/P | Pictures in local packs | L1, L2, M3, N11 |
| Text baked into pictures | part of the picture | Only by repainting the whole picture | None | H5 + N6 (list in §5) |
| Engine pictures `data/game_location.vga` | `GameLocationScreen` L70 | Key `plegacy/game_location` exists but is never exported | None | L6, N21 |
| Palette data `gfx/palette.e` (71 palettes) | `PaletteBank`; drives fades, swaps, seasons, water | – | – | M4, M5, N5 (HD art is fixed colour) |
| Not loaded by code: `back8`, `back40`, `back41`, `back99`, `blitmap6`, `menu.f`, `bo.f`, `euromap.f`, `permap.i`, `icons.96` | – | – | – | none needed unless used |

---

## 4. Every drawing call that drops HD or draws at 640x400 only

Grouped by mechanism (§1), callers by screen. "Flat and fine" calls (axis-aligned fills and
lines on surfaces that have no HD underneath) are listed in group C only where they make a
screen look 1994-flat next to HD art.

### A. No HD source: drawn at 640x400, magnified by nearest neighbour

| Call | Where | Screens |
|---|---|---|
| `Surface::from_indexed`, raw array, no `attach_hd` | CursorManager.cpp L38-42 | every screen |
| `RawRenderer::surface` (raw `.pc` bytes) | RawRenderer.cpp L11-16; callers CardGfx.cpp L16-36, CharacterPhotoBox.cpp L11-20, AlertPopup.cpp L59-64, ConfirmPopup.cpp L57-65 | turn card (PlayerTurnCard L36), staff (StaffView L886, L982, L1142), profile (L450), characters, alert/confirm |
| Engine picture without pack file | GameLocationScreen.cpp L70, L148 | game location |
| `fill_rect` per map tile | CityView.cpp L845-853 | city view FAR |
| `put` hatch dots | CityView.cpp L1174, L1209 (via `render_with_color` L911-961, `render_with_cell_color` L1004-1038) | city DISTRICTS, CONTROL |
| `draw::line` non-axis (Bresenham `put`) | Draw.cpp L35-67 | GlobalStats L1036; TutorialPopup L100-101 |
| `draw::line_2x` (2x2 blocks) | Draw.cpp L70-105 | MarketResearch L534-537; CheckboxMenuItem L49-54; CookBookPopup L56-74 |
| `draw::fill_circle` | Draw.cpp L175-185 | SliderMenuItem L77-78 (options) |
| per-pixel circle mask `put` | HalfSizePizza.cpp L37-45 | RecipePreview L39; CookBookPopup L150 |
| integer cursor position | PointerTracker.cpp L23-28; Renderer.cpp L305-308 (GPU), `draw_software_cursor` L486+ (CPU) | every screen, incl. furniture/topping cursors |
| motion rounded to engine px | GuestWalkScene.cpp L475-479; CarManager.cpp L90-120; CardSlider.cpp L98-101; ActorView.inl L289-315; AnimatedStats.cpp L328-349; cameras | restaurant, city, profile/staff, actors, animated stats |

### B. Plain overdraw that hides HD art underneath (patches)

| Call | Where | Screens |
|---|---|---|
| `fill_rect` patch over textured HD | PurchasingMain L31-34; StockMain L54; Newspaper L69-71, L118-121; Advertising L179-184, L302, L409; Contracts L222-235; CityView `render_tabs` L1126 | purchasing, stock, newspaper, advertising, contracts, city tabs |
| `fill` + `blit_raw` of flat text surfaces | Newspaper L91-96 → L132-135 | newspaper (article area) |
| `replace_colors` | StatBar.cpp L72-75 | staff cards, pizza info |
| colour-keyed `blit` from an HD source | BusyCursor.cpp L22-23 → Surface.cpp `blit` L396-436 / `hd_transfer` L156-260 | intro/loading (BusyCursor L39), every turn (TurnLockLayer L41-42) |
| `fill_rect` gradient bands | AnimatedStats L636-640 | capacity |
| opaque hover/selection bands over HD paper | RestaurantMenu L198, L241; IngredientOrderScreen L213; DepotIngredientOrderScreen L210; DepotExportScreen L126; DepotWeaponInterface L68; RestaurantView L1586; FurnitureListView L195; Advertising L317 | menu, orders, depot, weapon list, furniture list, newspaper ads |
| `blit_raw` of a raw sprite (copies its transparent pixels) | AnimatedStats L952 (bills) | animated statistics (behaves like 1994; noted only) |

### C. Code-drawn flat surfaces (sharp, but never remastered)

| Call | Where | Screens |
|---|---|---|
| `fill` paper + `draw::grid` | PageDecoration.cpp L17-26; GlobalStats L62-64, L482-483; RestaurantStats L41-44, L165-168; MarketResearch L188-190, L511-512, L812-814; AnimatedStats L256-265; StaffView L179-180 | all statistics screens, market research, staff |
| popup chrome | BasePopup L52-99; ContextMenuPopup L27-29, L115; TutorialPopup L82-84; TvPopup L62-85; JukeboxPopup L78-101; GuestTalkPopup L192-200; FinanceComponents L36-43; TextInputPopup L46; NumberInputPopup L66; ActorView.inl L334 | all popups, actor contract box |
| widgets | DropdownList L39-64; DropdownDecoration L22; ScrollBar L34-43; TextListBox L83-97; SaveFileListBox L41-62; SaveFileInputBox L35-88; SaveFileInfoBox L45-48; RecipeFileInfoBox L36-38; MissionTypeBox L24-42; CharacterSelectionBox L20-24; CompanySelectionBox L25-79; TooltipLayer.h L63-66; CategoryOverviewScreen L641-675; JudgeAnimator L114-128 | menus, city dropdowns, purchasing, judges |
| menu backgrounds | Menu.cpp L482-507; CharacterSelection L163-174; MenuGameLoad L233-237; MenuMissionSelect L247-250; MenuSaveAsGame L148-152; MenuSaveAsMission L135-139; MenuRecipeExport L128-130; MenuRecipeImport L222-224 | all sub-menus |

### D. Palette operations HD art cannot follow

| Call | Where | Screens |
|---|---|---|
| custom palette / cycling | CityView `render` L681, `update_water_palette` L1570-1579, `setup_water_animation` L1581-1609; PizzaDesign.cpp L344-347 | city view, pizza info |
| palette switched after HD art was drawn | StockMain L33; PurchasingMain L29; AnimatedStats L714 (and L541-543 frames in palette 40); GuestTalkPopup L229; MarketResearch L47 | stock, purchasing, animated stats, guest talk, market research |
| layer palette ≠ asset export palette (to measure, N5) | ActorView.inl L276; RestaurantView L1256 + CharacterRenderer L43; GlobalStats L62, L482; RestaurantStats L41, L826; MarketResearch L188, L499; AnimatedStats L367, L459, L783; StaffView L179; PopupManager L64-67 | actors, restaurant guests, statistics, staff, popups |
| HD text colour fixed at draw time | Font.cpp L297 (`target.colors()`); popup surfaces built in palette 0 (BasePopup L53, L72) | menu-mode popups (N4) |

### E. HD present but limited by pack content

MMPX sprites stay pixel art (HD_REMASTER.md L162-166); atlases upscaled whole (L7); feathered
alpha inside outlines (H2; hdpack.py `reference_mask` L146-151, `--feather` default 1.5 L818);
4x4-px med1 tiles (M6); lettering (H5, N6); and a pack below the target size is magnified
bilinearly without a warning (HdPack.cpp L185-193, N9).

---

## 5. Cross-check of `docs/hd-audit.md`

| ID | Verdict | Corrections and additions |
|---|---|---|
| H1 cursor | Confirmed (CursorManager.cpp L21-42) | Renderer refs drifted: GPU cursor is `render_gpu` L267-316 (`to_sdl_surface` L293-294), CPU `draw_software_cursor` L486+. Also snaps to the 640x400 grid (N3). |
| H2 sprite fringe | Confirmed (`render_push_pin` L140-151, `render_decorated_corners` L153-190, `reference_mask` L146-151, `hole_fraction` L266-275) | Compositor rule is now `draw_layer` L507-590 / `needs_base` L536-541 (not L325-348). Add: stacked bar slices repeat the soft edge every 2 px (N12). |
| H3 portraits | Confirmed | ConfirmPopup range is L57-65. Portraits keep palette 38 even on palette-0 popups. |
| H4 FAR map | Confirmed (L817-859, `fill_rect` L852, `blit_raw` L857, default L99) | The indexed far map follows the season via `palette_` (L107, L819); an HD fix must tint the same way (M4). |
| H5 baked text | Confirmed for back27, back21, back37, back30, back31, back26, back36, back28/blitmap5 | Add N6. Also the AI rule for `gfx/back*` uses denoise 0.35 (prompts.json L35-39), so these labels are garbled by the default comfy run, not only "kept blocky". |
| M1 flat patches | Confirmed (PurchasingMain L31-34, StockMain L54, Newspaper L69-71, Advertising L302, L409) | Missed the largest one: the newspaper article area (N1). Add Contracts L222-235 and Advertising L179-184 (N19). |
| M2 hatching | Confirmed (L1158-1216, `put` L1209) | – |
| M3 diagonals | Confirmed (Draw.cpp L35-67, L70-105, L175-185; all six callers) | The in-flight `Surface::hd_overlay` (Surface.cpp L125-147) + `HdCmd::tint` (HdList.h L22-25) are the right primitive for the fix (§6 F8). |
| M4 seasons/water | Confirmed (L177-189, L681, L1570-1609) | – |
| M5 palette mismatch | Confirmed (GuestTalkPopup L229, AnimatedStats L537-543, StockMain L33, PurchasingMain L29) | More pairs to measure (N5). |
| M6 medium tiles | Confirmed (L789-815; StatsRestaurants L130-146) | – |
| M7 motion | Confirmed (`drawn_position` L463-480) | Also cars, card slides, actor nod, stats strip scroll, and the cursor (N3). |
| L1 stat bars | Confirmed (L72-75) | `replace_colors` runs after the text, so it can also cut the HD letters (N20). |
| L2 half pizza | Confirmed (L37-45) | Callers: RecipePreview L39, CookBookPopup L150. |
| L3 busy cursor | Confirmed (L20-29) | Visibility is higher than "low": TurnLockLayer uses the same padlock on every turn pipeline run (N2). |
| L4 hardware cursor | Confirmed (L189-223, L196) | – |
| L5 capacity bars | Confirmed (L574-583, L603-607, L636-640) | Same pattern in the sales curve (N12). |
| L6 game location | Confirmed (L70) | The picture's lettering is rendered offline from the original fonts (`tools/gen_game_location_screen`), so even an upscaled PNG keeps 1994 letters (N21). |
| L7 atlases | Confirmed from tooling (no splitting in `upscale_pack` L199-263 or `comfy_pack` L597+) | Add floors/walls k0-k13 (restaurant tiling), spot/tvmisc frames, g* face cells, landsym ground tiles. |
| L8 unknown palettes | Confirmed: manifest flags 30 frames (typ10e, walk0, walk10) | – |
| "Checked and HD-correct" | Agreed | Except: fades are correct; season/water are not (M4); custom cursors keep HD art but snap (N3). |

### New findings

* **N1 Newspaper article area is a flat patch** (medium-high). `Newspaper::render` fills the
  title and body surfaces with the flat paper index (L91-96) and copies them with `blit_raw`
  (L132-135); with a picture, the side column is filled flat too (L118-121). About 316x270
  engine pixels of the HD paper are replaced by one colour. Fix: draw the text straight onto
  `ctx.surface` (the wrapping already works there), or fill the temporary surfaces with
  `TRANSPARENT` and `blit` them with the key; patch the side column from a clean area (F6).
* **N2 Padlock square on every turn** (medium). `TurnLockLayer` installs
  `icon_surface(PADLOCK)` (TurnLockLayer.cpp L20, L41-42), which is the L3 keyed blit. Fix F22.
* **N3 Cursor and custom cursors move on the 640x400 grid** (medium, every screen).
  `PointerTracker::on_motion` casts SDL's float render coordinates to int (L23-28); the
  renderer places the cursor at `cursor_pos * k` (Renderer.cpp L305-308). At 4K the arrow
  jumps 6 texels. Fix F1.
* **N4 HD text colour comes from the drawing surface's palette** (verify). `prepare_hd_line`
  resolves colours with `target.colors()` (Font.cpp L297); popups are composed on palette-0
  surfaces (BasePopup.cpp L53, L72; ContextMenuPopup L27; TutorialPopup L82) and shown in
  palette 38 in menus (PopupManager.cpp L64-67). Where palettes 0 and 38 differ at a text
  index, HD letters and the 1994 colours disagree. Fix F10.
* **N5 More palette-swap pairs to measure** (verify, same method as M5): typNe heads
  (palette 22) on back10-22 (palettes 22-34) (ActorView.inl L276); gaeste (16) on the
  restaurant layer (4) (RestaurantView L1256); stabo (42) under 7, 8, 43 (RestaurantStats
  L826, GlobalStats L62, L482); blitmap2/blitmap4 (7) under 8, 42, 43 (RestaurantStats L41,
  AnimatedStats L783); permap (0) cork under 7 and 13 (MarketResearch L499, StaffView L179);
  stanbobs (8) under 62, 7, 42 (MarketResearch L47, AnimatedStats L367, L783); back9 (4)
  under 7 (AnimatedStats L712-714); back4 (18) with indices 0xC0-0xDF taken from 13
  (PizzaDesign.cpp L344-347).
* **N6 More lettering baked into pictures** (medium): poster slogans in `plakat0..8`; banner
  lettering in the TV intro frames of `tvmisc`; the shirt slogan in `back20` (mafia); clock
  digits in `back24`; shop, bank and cinema signage in `landsym` building tiles; digits and
  letters inside `icons` cells; logo lettering in `menu`, `titel2`, `cyber`, `mplogolo`
  (`titel1` is already redrawn); and `data/game_location.vga` (N21).
* **N7 AI prompts mislabel whole families** (high for pack quality). `prompts.json`: every
  `gfx/back*` is "an empty restaurant room" (L35-39) though most are paper pages, actor
  scenes, a terminal, a cheque and a newspaper; `gfx/k*` floors and walls are "UI icons"
  (L60-64); `gfx/blitmap*` UI atlases are "room detail" (L40-44); `permap` button plates and
  cork are "a map" (L50-54); landsym, med1, zw*, ende, bo, orden, tvmisc fall to the default.
* **N8 Animation overlays cut from the same sheet** (medium). Guest faces draw eye and mouth
  cells over the face (GuestTalkPopup L211-222); actor heads draw the blink frame over the
  head (ActorView.inl L286-300); doors and TV frames are sheets. Per-image AI or whole-sheet
  upscaling does not keep overlay texels identical to the base, so blinking and talking show
  a patch.
* **N9 Low-resolution packs are magnified silently** (medium). `HdPack::decode` resamples any
  aspect-correct file to `engine size x k` (HdPack.cpp L185-193). A 6x pack (pack6 back1 is
  1152x1056) at `hd_scale 6` is magnified 2x bilinearly. `validate --min-scale` defaults to 2
  (hdpack.py L779). Fix: warn once per pack when the effective scale is below `2k`; validate
  against the intended hd_scale.
* **N10 Code-composed paper pages are flat colour** (medium; 7 screens). Statistics, market
  research and staff pages are `fill` + `grid` (§4 C) with HD pins and corners on top, so the
  HD sprites sit on 1994-flat paper.
* **N11 UI chrome has no HD skin** (low-medium). Popups, buttons in popups, list boxes,
  dropdowns, tooltips, scroll bars and hover bands (§4 C) are crisp but flat; hover bands are
  opaque over HD paper.
* **N12 Bars built from repeated 2-px slices** (low). `RestaurantStats::render_sales_curve`
  blits the bar sprite every 2 px (L917-929), as the capacity screen does (L5); soft HD edges
  stack into stripes.
* **N13 Export and pack keys cover only `.vga`/`.bob`** (enabler). AssetExporter.cpp L144-147;
  `HdPack` has `vga()` and `bob_frame()` only (HdPack.h L60-63). No keys for raw pictures,
  the arrow, engine pictures, or variants (season, palette, text-free, language).
* **N14 No HD coverage gate** (enabler). `tools/ci/hd_gates.py` checks GPU/CPU equivalence,
  state fingerprints and frame time in the restaurant only (L65-130). Nothing measures how much
  of each screen still comes from the indexed base.
* **N15 Depth and normal maps are unused** (optional). `depthmaps.py` writes them; `LightPass`
  draws 2D radial gradients with no occlusion or normal shading (LightPass.cpp `apply`), and
  only `RestaurantView` is lit (RestaurantView.h L69).
* **N16 HD_REMASTER.md is stale on text**: L28-30 says fonts are drawn from the original font
  scaled up; L225-227 says the indexed pixels still get the original letters. Font.cpp
  L657-660 draws HD letters only (CHANGELOG is right).
* **N17 Display fit** (low). The 16:10 HD frame is minified 0.9x with LINEAR on a 3840x2160
  screen (Game.cpp L1209); `hd_scale` is not chosen from the display.
* **N18 City close zoom repetition** (low-medium). 16x16 landsym tiles repeat on a 32-engine-px
  grid; at 4K the repetition and the atlas seams read as a pattern.
* **N19 More flat patches**: stacked pages on the contracts picture (Contracts L222-235) and
  the selected-channel block on the advertising overview (Advertising L179-184).
* **N20 StatBar can cut HD letters** (low). `replace_colors` (L73-75) runs after the label and
  value are drawn (L66-70) and hides every HD command, letters included, on matching pixels.
* **N21 Game-location lettering is offline-baked** (low). The fallback text path
  (GameLocationScreen L153-172) needs the game's `.fnt` files, which are not available on this
  screen; the HD typefaces in `data/fonts` would be.

---

## 6. Fixes (state of the art, effort, risk)

| Fix | Closes | Approach | Effort | Risk |
|---|---|---|---|---|
| F1 HD cursor | H1, N3, L4 | Trace the 11x11 arrow to a polygon once; rasterize it anti-aliased (analytic coverage or SDF) at `22k` per mode palette and `attach_hd`; or pack keys `plegacy/cursor.png`, `cursor_menu.png`. Keep the float position from SDL beside the int one (PointerTracker) for drawing only; hit-testing stays integer. Hardware path: `to_sdl_surface(src, 1, k)` then LINEAR. | S | Low |
| F2 Alpha normalisation + vector UI sprites | H2, N12 | At load (`Bob::frame`, `VgaLoader::load`) set alpha 255 inside the indexed outline and keep soft alpha only outside; tool: `alpha = max(nearest, blurred)`, validate alpha < 250 inside. Redraw pins, corners, arrows, coins, bars (stanbobs 26-40, lebbo, stabo) as SVG rendered at 12x. | S + M | Low |
| F3 Portraits | H3 | Export `chr/c{N}e.png` (palette 38); add `HdPack::picture(key, size)` and `RawRenderable::hd_key()`; `RawRenderer::surface` attaches. Art: SDXL/Flux img2img at denoise 0.3-0.4 with an identity adapter (IP-Adapter FaceID / InstantID) and one style seed, face restoration (CodeFormer), side-by-side review with `review_pairs.py`. Doubles reuse the base id. | M | Medium (likeness) |
| F4 Procedural city maps | H4, M6, N18 | Build per-tile HD thumbnails from HD landsym by area averaging (192→12 texels FAR, 192→48 MEDIUM at k=6), compose `320k x 240k` / medium caches once per city and season, `attach_hd`. Optional: blend toward `palette_map` colours so the 1994 colour coding stays readable. For CLOSE, add 2-4 variants per ground tile (pack keys) chosen by a tile hash to break repetition. | M | Low |
| F5 Clean plates + re-typeset | H5, N6, N21 | Inpaint lettering out of each listed picture (LaMa or SDXL inpaint, then manual cleanup) as `<key>.clean.png`; a label table (`data/hd_labels.json`: key, rect, string id, face, colour, shadow, align) drawn with `HdTypeface` after load, same placement rules as `prepare_hd_line`, so it follows `hd_text` and can be localised. Logos (menu, titel2, cyber, mplogolo) and posters get hand-finished vector lettering like titel1. Game location: draw with `HdTypeface` directly. | M-L | Medium (font match) |
| F6 HD-aware patching | M1, N1, N19 | Newspaper: no flat fill; draw text on the screen surface or with transparent temporaries. Other patches: copy a clean area of the same HD picture (`blit` of a source rect, tiled) or use `.clean` variants; stop sampling the indexed colour (PurchasingMain L31). | S | Low |
| F7 Analytic hatching | M2 | Rasterize the 8-px diagonal hatch anchored at `pattern_offset_x/y` as a white coverage mask per viewport and push it with `hd_overlay` + `tint` over flat cells. | S-M | Low |
| F8 AA primitives | M3, L2, L5 | In `gfx::draw`, when k > 1, keep the indexed write and also rasterize the stroke/disc/ring anti-aliased (Wu or exact coverage) into a cached mask; push it with the in-flight `Surface::hd_overlay` + `HdCmd::tint`. Capacity: one stretched slice + HD vertical gradient. | M | Low |
| F9 Palette-aware HD | M4, M5 | Per (key, displayed palette) build variants once at load: for each block whose index colour changes, multiply texels by `new_rgb / export_rgb` (index known from the indexed surface); season variants for landsym/med1, four small water overlays swapped by `frame_index`. Longer term: store an HD index map (edge-aware upscale of the original indices) with the art so any palette effect can be applied exactly. | M | Medium (colour shifts) |
| F10 Palette trace + resolve at composition | M5, N4, N5 | A debug "palette trace" logs (asset key, layer palette at upload) during a scripted tour of every `nav` screen; export variants for the pairs that differ. For HD text and masks, store the palette index in `HdCmd` and resolve it in `HdCompositor` with the layer palette (also makes palette swaps and seasons apply to HD text). | S-M | Low |
| F11 Sub-pixel placement | M7, N3 | Let sprite blits carry a fractional texel offset (`HdCmd.dx/dy` are already texels); float cameras for restaurant and city; indexed copy and floor bookkeeping stay at the rounded position. | L | Medium (clipping, floors) |
| F12 Atlas-aware tooling | L7, N8, N18 | Split sheets into cells from known grids and `gfx/offsets*`; pad by edge repeat (wrap for tileable floors/ground); upscale per cell; reassemble. Generate overlay cells (eyes, mouths, blink frames) by inpainting inside the HD base, so they match it exactly. | M | Low |
| F13 Prompt families | N7 | One rule per real family (paper pages, actor scenes, floors/walls, UI atlases, city tiles, milestones); denoise ≤ 0.15 or no diffusion for anything with lettering; ControlNet tile/lineart for UI atlases. | S | Low |
| F14 Consistent animation frames | N8, H2 | Key-frame redraw plus propagation (EbSynth-style or a video diffusion model with temporal attention) for typ*, walk*, gaeste, night*; or MMPX base + painted key frames by hand. | L | Medium |
| F15 HD UI skin | N10, N11 | Pack keys for a tileable paper texture and 9-slice frames (`plegacy/ui/paper.png`, `popup.png`, `button.png`, `listbox.png`, `tooltip.png`), drawn as HD under the same rects; hover/selection as translucent tinted overlays instead of opaque `fill_rect`. Grid lines stay crisp. | M | Low |
| F16 Exporter and pack API | N13, H3, L6 | Export chr portraits, the arrow, engine pictures; `HdPack` keys for raw pictures and variants (`<key>.<season>`, `<key>.p<pal>`, `<key>.clean`, `<key>.<lang>`). | M | Low |
| F17 HD coverage gate | N14 | Console command `hdcov`: per layer, share of engine pixels whose visible content is the indexed base (floor plain, no HD command) plus a heatmap PNG; `hd_gates.py` runs it over every `nav` screen in `screens.md` and fails on regressions. | M | Low |
| F18 Pack resolution warning | N9 | Log the effective scale per pack and refuse/flag files below `2k x` original in `validate --hd-scale k`. | S | Low |
| F19 Normal-mapped lighting | N15 | Use the normal/depth maps for per-light N·L and simple occlusion; needs SDL_GPU shaders (or a CPU light pass at engine resolution). Optional. | L | Medium |
| F20 Display fit | N17 | Pick `hd_scale` to fit the display, or minify with a two-step box filter. | S | Low |
| F21 Small fixes | L1, N20, L2, L5 | StatBar: mask from `bar_empty_` corners before the text; half pizza and capacity via F8. | S | Low |
| F22 Keyed HD transfer | L3, N2 | In `Surface::blit` with `transparent != 0` and an HD source, materialize the rect and zero alpha where the source index equals the key. | S | Low |
| F23 Docs | N16 | Update HD_REMASTER.md L28-30 and L225-227 to match Font.cpp. | S | None |

---

## 7. Gaps to close for a true end-to-end HD remaster (ranked)

Ranked by how often the player sees it, then by effort.

1. **Mouse cursor** (H1, N3, L4): on every screen, every frame, and it snaps to 640x400. F1, S.
2. **City view** (H4, M2, M4, M6, N18): the hub screen opens at the non-HD FAR map; DISTRICTS
   and CONTROL are hatched pixels; seasons and water never reach HD. F4 + F7 + F9, M.
3. **Portraits** (H3): turn card every turn, staff, profile, character selection, popups.
   F16 + F3, M.
4. **Pack quality at the source** (N7, L7, N9, H2): prompts that describe the wrong subject,
   atlases upscaled whole, packs magnified silently, alpha that lets the staircase show.
   F13 + F12 + F18 + F2, S-M; every screen benefits.
5. **Baked lettering** (H5, N6, N21): advertising, city dropdowns, capacity, charts,
   newspaper, cheque, posters, TV banners, mafia shirt, city signage, logos. F5, M-L.
6. **Flat patches over HD art** (N1, M1, N19): the newspaper article area first. F6, S.
7. **Padlock square on every turn** (N2, L3). F22, S.
8. **Palette mismatches** (M5, N5, N4): measure with the palette trace, then variants or
   composition-time resolve. F10 + F9, S-M.
9. **Stair-stepped primitives** (M3, L2, L5, L1, N20, N12): checkboxes, sliders, chevrons,
   economy graph, pizza rim, capacity and sales bars. F8 + F21, M.
10. **Tiny UI sprites** (H2): pins, corners, arrows, coins as vector redraws. F2, M.
11. **Animation consistency** (N8): blink/talk overlays, actor and walk cycles. F12 + F14, M-L.
12. **1994-flat code surfaces** (N10, N11): paper pages, popups, list boxes, hover bands.
    F15, M.
13. **Sub-pixel motion and scrolling** (M7). F11, L.
14. **Enablers**: exporter/pack API (N13, F16) and an HD coverage gate (N14, F17), so every
    item above stays closed.
15. **Remainder**: game location (L6, N21), display fit (N17), palettes of typ10e/walk0/walk10
    (L8), stale docs (N16), normal-mapped lighting (N15, optional).

---

## Notes and uncertainties

* N4 and N5 name palette pairs that differ by handle; whether the colours differ at the
  indices those pictures use needs the M5 measurement against `GFX/PALETTE.E`, which this
  audit did not read (only a file listing of the game folder was allowed).
* Visibility judgements for N8, N12, N17 and N18 come from code and tooling, not from
  rendered frames.
* The matrix counts treat "partial" cells conservatively: a row is "fully HD" only when every
  non-cursor column is Y or –.
* The in-flight HD text change (glyph sheet + `HdCmd::tint`) does not change any verdict here;
  it keeps resolving colours at draw time (Font.cpp L297), so N4 still applies.
