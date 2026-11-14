# Feature gap analysis: Pizza Tycoon HD vs. 2026 remasters

Written 2026-11-14 by reading the source and docs in this checkout. The C++ was not built
or run. Research comes from the web pages linked in [Sources](#6-sources).

Line numbers are as read on that day. `src/engine/Renderer.cpp` and
`src/engine/HdCompositor.cpp` were changing while this was written, so check the numbers
again before wiring anything in.

**Ground rule.** Classic gameplay stays exactly as it is. Every feature below changes only
presentation, comfort, accessibility or quality of life. Each one lives in `GameOptions`,
never in `GameRules` (rules are stored in saves, README.md:124-129), and must pass the
existing proof that game state is unchanged (`tools/ci/hd_gates.py` fingerprint gate,
CHANGELOG.md:48-52). "Expanded" mode is out of scope here (see
`docs/blueprint/sdk-roadmap.md`).

**Legend.**
* **Y**: documented in the cited source.
* **—**: not found in the sources read. The feature may still exist.
* **Priority:** **P0** is needed to call the remaster state of the art. **P1** is the next
  release. **P2** is later or optional.
* **Effort** (one engineer or agent): **S** is a day or less. **M** is 2-5 days. **L** is more
  than a week.

## 1. What the game has today

| Surface | Where | Notes |
|---|---|---|
| Options struct | `src/core/state/GameOptions.h:145-215` | 30 presentation and comfort fields, plus `rules` |
| Settings file | `src/core/util/ConfigManager.cpp:124-222` (`option_defs`) | `settings.conf` lists every option with its default. Only changed values are written (`:321-334`) |
| Options menus | `src/views/screens/menus/MenuOptions.cpp` | Five screens: Audio `:140-168`, Graphics `:170-253`, Controls `:379-442`, Gameplay `:255-298`, Rules `:300-377`. Advanced rows are hidden behind a checkbox (`:444-465`) |
| Screens enum | `src/core/state/UIState.h:71` | `OptionsScreen { Audio, Graphics, Controls, Gameplay, Rules }` |
| Bindings | `src/engine/input/DefaultBindings.cpp:26-116`, `BindingsFile.cpp:66`, `MenuBindings.cpp:155-200` | Every action can be rebound to keys or pad buttons. `bindings.conf` template. Restore defaults |
| Command line | `docs/blueprint/README.md:132-151`, `src/Main.cpp:77-170` | `--hd-scale`, `--hd-pack`, `--crt`, `--config`, `--exec`, `--export-assets` and others |
| Saves | `src/core/save/SaveGameManager.h:26-86` | Format v3, metadata block for the load list, rotating autosaves, high scores, state fingerprint |
| Per-user files | `src/engine/PlatformPaths.cpp:19-55` | Always `SDL_GetPrefPath`: settings, bindings, saves, screenshots, recipes, missions |

HD settings that **exist but are not in any menu**:
* `hd_scale` (1-8) and `hd_pack` (`GameOptions.h:179-184`, `ConfigManager.cpp:146-153`) are
  read once at startup. They are set only in `settings.conf` or on the command line.
  `build_graphics` (`MenuOptions.cpp:170-253`) has no row for either.

## 2. Comparison matrix

| Feature | Pizza Tycoon HD today (evidence) | D2R / SC:R / MI:SE | C&C Remastered | AoE II DE | OpenTTD | OpenRCT2 / CorsixTH | Pri | Eff |
|---|---|---|---|---|---|---|---|---|
| **Graphics** | | | | | | | | |
| Render resolution | `hd_scale` 1-8, 6 = 3840x2400, restart, config/CLI only (`GameOptions.h:183`, `Game.cpp:1214-1216`) | — | Y 4K [r5] | — | Y resolution, refresh rate [r9] | Y resolution, window scale [r11] | P0 | S |
| Auto-match the display | No: default `hd_scale = 1` | — | — | — | Y interface "auto-detect" [r9] | — | P0 | S |
| Window modes | Resizable window, borderless fullscreen (`Game.cpp:1119-1123`), Ctrl+F (`DefaultBindings.cpp:31`); no Alt+Enter/F11 | — | — | — | Exclusive only, no borderless [r9] | Y window/fullscreen/borderless [r11] | P1 | S |
| Scaling filter, integer scaling | HD frame always LINEAR to window (`Game.cpp:1208-1210`), letterbox only (`Game.cpp:923-925`); MMPX "Smoothing" at restart (`GameOptions.h:178`) | — | — | — | — | Y drawing engine choice [r11] | P1 | S |
| CRT look | 4:3 aspect stretch only (`Renderer.cpp:725-776`); no scanlines; engine avoids custom shaders by design (HD_REMASTER.md:125-126) | — | — | — | — | — | P1 (overlay) / P2 (shader) | S / M |
| Instant classic/HD toggle | No: art pack is fixed at startup. `hd_text` and `lighting` do switch live (`MenuOptions.cpp:228-245`, `Game.cpp:615-616`) | Y D2R `G` [r1]; SC:R `F5` [r2]; MI:SE any time [r3] | Y `Space` [r4] | — | Base graphics set chosen in options [r9] | — | **P0** | M |
| Classic vs HD compare slider | No (only a static `screenshots/hd_compare.png`) | — | — | — | — | — | **P0** | M |
| Frame-rate cap | 30/60/120, separate from the 30 Hz simulation tick (`GameOptions.h:16-30,185`) | — | — | — | Y refresh rate [r9] | Y uncap FPS, show FPS [r11] | P1 | S |
| VSync | On/off, changes live (`Game.cpp:459-465`) | — | — | — | Y (restart) [r9] | Y [r11] | P2 (adaptive) | S |
| HDR output | No (SDR sRGB); SDL3 can output `SRGB_LINEAR` on D3D11/D3D12/Metal [r26] | — | — | — | — | — | P2 | L |
| Smooth motion | Y, and proven not to change game state (`GameOptions.h:186-188`, CHANGELOG.md:48-52) | — | — | — | — | — | done | — |
| Lighting, day/night | Y, HD only (`GameOptions.h:189-191`, CHANGELOG.md:15-22) | — | — | — | — | Y day/night, lighting, weather toggles [r11] | done | — |
| Reduce flicker/motion | No: flames, screens and the jukebox always flicker (CHANGELOG.md:17-19) | — | — | — | — | CorsixTH: screen shake off [r14] | P1 | S |
| **UI scaling, widescreen** | | | | | | | | |
| UI or text scale | The whole 640x400 screen scales with the window. No separate HUD or text scale, because the layout is fixed | — | — | Y HUD up to 125%, tooltip scale [r8] | Y 1x-5x [r9] | Y window scale [r11]; CorsixTH 2x/3x [r14] | P1 (magnifier) | M |
| Widescreen | 16:10 frame pillarboxed on 16:9; 4:3 CRT option (`MenuOptions.cpp:171-181`) | — | — | — | — | — | P2 (border art) | S |
| Handheld fit | 640x400 x2 = 1280x800, an exact fit for Steam Deck. The `MINI` font is drawn at 1x (`FontLoader.h:36-38`) | — | — | — | — | — | P1 | M |
| **Text** | | | | | | | | |
| Modern / readable typefaces | HD text: OFL typefaces, live toggle (HD_REMASTER.md:207-234). No hyperlegible or dyslexia choice | — | — | — | Y sprite font vs TrueType, anti-alias [r9] | CorsixTH: Unicode TTF, text shadow [r14] | P1 | S |
| Text size | None: lines are narrowed to fit the 1994 boxes (HD_REMASTER.md:222-225) | — | — | Y tooltip scale [r8] | Y [r9] | Y [r11] | P1 (magnifier) | M |
| Contrast / text plates | None | — | — | Y "Readability Panels" [r8] | — | — | P1 | M |
| Text colour styles | Original palette colours only | — | — | — | — | — | P2 | S |
| **Colour-blind** | | | | | | | | |
| Colour-blind modes | None. Player colours are fixed palette indices (`CityView.cpp:993`); the only option shows them in dropdowns (`GameOptions.h:207`) | — | — | Y deuteranopia, protanopia, tritanopia, unique player colours [r8] | — | — | P1 | M |
| **Input** | | | | | | | | |
| Rebinding | Y keyboard and pad, every action, file plus menu (`MenuBindings.cpp:155-200`) | — | — | — | — | Y shortcut keys [r11]; CorsixTH hotkeys window [r13] | done | — |
| Controller | Pad works in menus and the burglary scene only; it cannot move the pointer (`Gamepad.h:13-50`, `DefaultBindings.cpp:58-73`) | — | — | — | — | — | P1 | M |
| Cursor | Software/hardware cursor (`GameOptions.h:176`). The arrow is not HD (hd-audit.md:55,80-98). No size or colour option | — | — | — | — | Y trap cursor [r11]; CorsixTH capture mouse [r13] | **P0** (HD arrow, size) | S |
| Mouse | Invert RMB drag, invert wheel (`GameOptions.h:174-175`, `Game.cpp:777`) | — | — | Y scroll speed, inertia [r8] | — | Y invert RMB, zoom to cursor [r11] | done | — |
| Edge scrolling | Original click-and-hold on the wood frame (`ScrollEdges.h:25-53`), RMB drag, Shift for faster (MODERNIZATIONS.md:33) | — | — | Y scroll speed [r8] | — | Y scroll at screen edge [r11] | P1 | S |
| **Audio** | | | | | | | | |
| Volume per channel | Sound and music sliders; map/restaurant FX toggles (`GameOptions.h:157-161`). Master volume fixed at 0.8 (`AudioManager.h:157`) | — | — | — | Y [r9] | Y master, effects, music [r11] | P1 | S |
| Mute when unfocused | No (focus loss only ends a drag, `Game.cpp:797-800`) | — | — | — | — | Y [r11] | P1 | S |
| Classic/remastered music, jukebox | Original CMF songs only | SC:R: classic or new audio, set separately [r2]; MI:SE orchestral score [r3] | Y remastered and classic tracks, jukebox [r4][r5] | — | Y music set, switchable in game [r9] | Y title music choice [r11] | P1 | M |
| Synth choice (OPL / MT-32 / GM) | libADLMIDI 1.6.1, one OPL3 chip, emulator never set (`AudioManager.cpp:49-58`). The data is CMF only, so MT-32 and GM do not apply (file-formats.md:254-259) | — | — | — | — | CorsixTH: MIDI devices [r14]; OpenRCT2: audio device [r11] | P1 (emulator) | S |
| Mono, output device | No: default device only (`AudioManager.cpp:67`) | — | — | — | — | Y device [r11] | P2 | S |
| **Speed and pause** | | | | | | | | |
| Game speed | 1/2/3/5/10x on Ctrl+1..5 (`DefaultBindings.cpp:33-37`) with an "Nx" caption (`GameInfoLayer.cpp:40-44`). Keyboard only; nothing below 1x | — | — | Y slow/casual/normal/fast [r8] | — | — | P1 | S |
| Pause | Ctrl+P popup (`PauseLayer.cpp:17-29`); no pause on focus loss | — | — | — | — | Y minimise on focus loss [r11] | P1 | S |
| **Saves** | | | | | | | | |
| Autosave | Off/daily/weekly/monthly, 1-9 rotating slots (`GameOptions.h:171-173`, `SaveGameManager.cpp:188-214`) | MI:SE: autosave [r3] | — | — | Y real-time interval [r9] | Y amount, frequency [r11]; CorsixTH [r13] | done | — |
| Quick save / load | No action for it (`Action.h:11-90`) | — | — | — | — | — | P1 | S |
| "Continue" on main menu | No (`MenuMain.cpp:27-52`) | — | — | — | — | — | P1 | S |
| Cloud saves | No: per-user folder only (`PlatformPaths.cpp:20`) | — | — | — | — | — | P2 | M |
| Portable mode | No (`PlatformPaths.cpp:19-39`) | — | — | — | — | — | P1 | S |
| **Help** | | | | | | | | |
| Tooltips | Y QoL tooltips, adjustable delay (MODERNIZATIONS.md:143-144, `GameOptions.h:206`) | — | — | Y scale, position, narrated [r8] | — | — | P2 (scale) | S |
| Tutorial | Y interactive (`MenuNewGame.cpp:62`, `src/core/tutorial/TutorialSteps.cpp`) | MI:SE: hint system [r3] | — | — | — | — | done | — |
| Encyclopedia / manual | Cook book for standard pizzas only (screens.md:155); manual is online (README.md:102-103) | — | — | — | — | — | P2 | M |
| Message history | Data exists: `StatusHub::ephemeral_log()` "for future scroll list UI" (`StatusHub.h:129-132`). No view | — | — | — | — | CorsixTH: adviser history, Ctrl+H [r14] | P1 | S |
| Screen narration | No | — | — | Y menu and tooltip narration [r8] | — | — | P2 | L |
| **Localization** | | | | | | | | |
| Languages | English DOS data only (`GameLocationContent.h:29`); engine strings are hard-coded English | — | — | — | Y [r9] | Y language, units, currency [r11]; CorsixTH [r13] | P2 | L |
| **Photo mode / screenshots** | | | | | | | | |
| Screenshot | Ctrl+S saves a PNG of the full-resolution HD frame (`DefaultBindings.cpp:30`, `Renderer.cpp:698-723`, `Screenshot.cpp:43-68`) | — | — | — | — | Y giant screenshots [r11]; CorsixTH timestamped PNG [r14] | P2 (no cursor, compare shot) | S |
| **Achievements, statistics** | | | | | | | | |
| Achievements | High scores and the original medals only (`SaveGameManager.h:72-73`, screens.md:90) | — | — | — | — | — | P2 | M |
| Statistics | All the original statistics screens (screens.md:94-95,124-126) | — | — | — | Y opt-in survey [r10] | — | done | — |
| **Modding** | | | | | | | | |
| Art replacement | Y HD packs for any VGA picture or BOB frame, plus tools (HD_REMASTER.md:76-101). No in-game pack picker | — | Y Workshop [r6] | — | Y base graphics/sound/music sets [r9] | OpenRCT2: JS plugins [r12] | P1 (picker) | S |
| Data / code mods | Values in `data/*.json`; no new entries (sdk-roadmap.md:19-35) | — | Y GPL source [r7][r30] | — | — | Y plugins [r12] | P2 | L |
| Editors | Save as mission (screens.md:80); recipe import/export (README.md:118-122) | — | Y map editor [r4][r5] | — | — | — | P2 | L |
| Workshop | Does not apply: no Steam release, and the game needs the user's own files | — | Y [r6][r7] | — | — | — | P2 | — |
| **Platform** | | | | | | | | |
| Log file | No: log goes to stdout and the in-game console only (`Game.cpp:139,295`). The Windows build has no console (CHANGELOG.md:105-107), so its warnings are lost | — | — | — | — | — | **P0** | S |
| Crash reporting | No handler, no minidump | — | — | — | — | — | P1 (local dumps) | S |
| Telemetry | None (the right default) | — | — | — | Y opt-in survey with preview [r9][r10] | — | P2 | M |
| Auto-update | None | — | — | — | — | CorsixTH: "check for updates" toggle [r13] | P2 | S |
| Branding | Window title is "Pizza Legacy {ver} [fps]" (`Game.cpp:648-652,893`) | — | — | — | — | Y show-FPS option [r11] | P1 | S |

Return to the Roots (Settlers II) also keeps the original's content, with higher resolutions
and stepless zoom, and puts its extras behind optional addons [r15]. That is the same
"classic plus opt-in" split this project uses.

## 3. Features that need an engine hook, not just a menu toggle

Each row needs new code at the named location. A plain `GameOptions` field with a menu row
is not enough.

| Feature | Hook location | Why |
|---|---|---|
| Classic/HD "Look" toggle | `HdCompositor::draw_layer`, step 3 (`src/engine/HdCompositor.cpp:552-590`): skip art commands, but keep text commands (`HdCmd::tint != 0` or `img->atlas`, `HdList.h:23-25`, `HdImage.h:41-44`). Redraw with `Renderer::redraw_all()` (`Renderer.h:53`); `invalidate()` only recomposes | HD art is baked into each layer's texture when that layer is drawn |
| Compare slider | Same loop, inside `SDL_SetRenderClipRect`. Lit layers: split `tex` and `shown` when `Renderer::render_gpu` recomposites (`Renderer.cpp:250-270`). New `CompareLayer` in `Overlays` (`Game.cpp:97-108`) | The split has to happen per layer before compositing |
| Auto render scale | `Game::configure_hd` (`Game.cpp:1214-1216`), before `init()` (`Game.cpp:219`): `SDL_GetDesktopDisplayMode` | Scale is fixed at startup; pack art is resized for it on first use (HD_REMASTER.md:87-91) |
| Integer scaling, pixel-art filter | `SDL_SetRenderLogicalPresentation` (`Game.cpp:544-546`, `923-925`) to `INTEGER_SCALE`; `create_display_texture` (`Game.cpp:1208-1210`) to `SDL_SCALEMODE_PIXELART` (SDL 3.4 [r25]; the project requires 3.4.0, CMakeLists.txt:192) | The presentation mode is set when the window is created |
| HD cursor, cursor size | `CursorManager::create_cursor_surface` (hd-audit.md H1); cursor rectangle in `Renderer::render_gpu` (`Renderer.cpp:273-322`); hardware path `hw_apply_cursor` (hd-audit L4) | The arrow is an 11x11 array with no HD image |
| CRT scanlines | `Renderer::render_gpu` before present (`Renderer.cpp:323-328`): draw a tiled pattern texture with `SDL_BLENDMODE_MOD`. A full CRT shader needs `SDL_CreateGPURenderState` [r24], which needs the GPU renderer instead of `SDL_CreateWindowAndRenderer` (`Game.cpp:893`) | Post-processing of the final frame |
| Magnifier | `Renderer::render_gpu` after the frame is drawn (`Renderer.cpp:323-328`): draw a zoomed source rectangle around the cursor | Final-frame effect |
| Text plates | `HdCompositor::draw_layer`: before each text command, `SDL_RenderFillRect` with alpha | Text exists only as HD commands |
| Colour-blind player colours | `PLAYER_CONTROL_COLORS` (`CityView.cpp:993`); patterns in `draw_cell_with_pattern` (`CityView.cpp:958,1035`); `CompanySelectionBox.cpp:46-58` | Colours are compiled-in palette indices |
| Reduce flicker | `Renderer::render_gpu` relight check (`Renderer.cpp:228-230`) and the time passed to `LightPass::apply` (`:257-258`) | Flicker comes from the per-frame time |
| Quick save / load | New `Action`s (`Action.h`), defaults (`DefaultBindings.cpp`), `GlobalActionHandler` callbacks (`GlobalActionHandler.h:19`, wired at `Game.cpp:362-390`), guarded by UI state | It must only save at safe points |
| Gamepad pointer | `InputManager` stick handling (`InputManager.cpp:92-101`) feeding `PointerTracker` and `InteractionManager::process` (`Game.cpp:783`) | No synthetic pointer exists |
| OPL emulator, master volume | `AudioManager::init` (`AudioManager.cpp:49-58`) calls `adl_switchEmulator` (`third_party/libadlmidi/include/adlmidi.h:710-741`); mixer `master_volume_` (`AudioManager.cpp:333-335`) | The mixer has no options today |
| Music pack | `AudioManager` music slot playback (`AudioManager.cpp:220-260`) plus a decoder. `third_party/` has only `stb_truetype`, not `stb_vorbis` | New stream type |
| Focus loss (mute, pause) | `Game::handle_events`, `SDL_EVENT_WINDOW_FOCUS_LOST/GAINED` (`Game.cpp:797-800`) | Event plumbing |
| Portable mode, log file, crash dumps | `platform_paths::init` (`PlatformPaths.cpp:19-39`); `Logger::add_sink` (`Game.cpp:139`); `main` (`Main.cpp:232-234`) | Runs before any options are loaded |

## 4. HD Remaster options page

### 4.1 Placement

* Add `OptionsScreen::Remaster` (`UIState.h:71`), a `> HD Remaster` entry at the top of
  `build_chooser` (`MenuOptions.cpp:103-128`), a `build_remaster()` function, and `nav op hd` in
  `src/core/debug/commands/NavCommands.cpp:45-54`.
* Move Lighting, HD text, Smooth motion and Smoothing from Graphics to this page. Graphics keeps
  window, aspect, FPS and VSync, which this page shows too.
* **Dense control-panel layout.** Two columns of about 12 px rows. Open the right column with a
  `PositionMarkerMenuItem`, as `add_back` already does (`MenuOptions.cpp:130-138`). Between
  y=48 and y=318 that fits about 22 rows per column. Use the existing widgets:
  `ToggleMenuItem`, `EnumMenuItem`, `SliderMenuItem`, `IntSliderMenuItem` and `LabelMenuItem`
  for group headers. Help text stays in its existing place below the list
  (`Menu::render_help_text`).
* Restart-only rows are marked **R**. When you leave the page with a changed R row, it offers
  to restart.

```
 OPTIONS - HD REMASTER                                Preset [HD (default)      v]
 -------------------------------------------------------------------------------
 LOOK & ART                            | DISPLAY
  Look ............ [HD     ] F7       |  Render scale ..... [Auto (6x)   v] R
  HD pack ......... [hd         v] R   |  Window ........... [Borderless  v]
  Pack coverage ... 1,212/1,699  71%   |  Aspect ........... [16:10       v]
  HD text ......... [On ]              |  Scaling filter ... [Smooth      v]
  Typeface ........ [Remaster   v] R   |  Integer scaling .. [Off]
  Smooth motion ... [On ]              |  Frame cap ........ [Display     v]
  Smoothing (1x) .. [Off        v] R   |  VSync ............ [On ]
                                       |  Show FPS ......... [Off]
 LIGHT & EFFECTS                       | ACCESSIBILITY
  Lighting ........ [On ]              |  Cursor size ...... [1x  v] Outline [Off]
  Light flicker ... [On ]              |  Text plates ...... [Off]  [=====---] 60%
  CRT filter ...... [Off        v]     |  Player colours ... [Default     v]
  CRT strength .... [===-------] 30%   |  Magnifier ........ [Hold Z ] [2x v]
 -------------------------------------------------------------------------------
 COMPARE [Off   v]   1994 |=========[||]=========| HD   50%    F8   > Preview
 -------------------------------------------------------------------------------
 <help text of the selected row>                                    R = restart
 Defaults                                                                  Back
```

### 4.2 Settings and defaults

| Group | Setting (config key) | Values | Default | Live? |
|---|---|---|---|---|
| Look & art | Look (`look`) | HD, Classic | HD | live, F7 |
| | HD pack (`hd_pack`) | `hd` next to the exe, folders in `<prefpath>/packs/`, None | `hd` if present | R |
| | Pack coverage | read-only: pack files found / 1,699 | — | — |
| | HD text (`hd_text`) | On, Off | On | live |
| | Typeface (`hd_typeface`) | Remaster (the current five OFL faces), Hyperlegible (Atkinson Hyperlegible, OFL [r28]) | Remaster | R |
| | Smooth motion (`smooth_motion`) | On, Off | On | live |
| | Smoothing (`smoothing`) | Off, Text, Text and graphics (used at scale 1) | Off | R |
| Display | Render scale (`hd_scale`) | Auto, 1-8 (shows the resulting size) | **Auto** (currently 1) | R |
| | Window (`fullscreen`, `double_size`) | Window, Window 2x, Borderless | Window 2x | live |
| | Aspect (`crt_aspect_ratio`) | 16:10, 4:3 CRT | 16:10 | live |
| | Scaling filter (`present_filter`) | Smooth (linear), Sharp (pixel art), Nearest | Smooth if HD, Sharp at 1x | live |
| | Integer scaling (`integer_scaling`) | On, Off | Off | live |
| | Frame cap (`render_fps`) | 30, 60, 120, 144, 165, 240, Display, Unlimited | Display | live |
| | VSync (`vsync`) | On, Adaptive, Off | On | live |
| | Show FPS (`show_fps`) | On, Off | Off (today the FPS is always in the title) | live |
| Light & effects | Lighting (`lighting`) | On, Off | On | live |
| | Light flicker (`light_flicker`) | On, Off | On | live |
| | CRT filter (`crt_filter`) | Off, Scanlines, Scanlines + mask | Off | live |
| | CRT strength (`crt_strength`) | 0-100% | 30% | live |
| Accessibility | Cursor size (`cursor_scale`) | 1x, 1.5x, 2x, 3x; Outline on/off | 1x, off | live |
| | Text plates (`text_plates`, `text_plate_opacity`) | Off, On; 20-100% | Off, 60% | live |
| | Player colours (`player_palette`) | Default, Deuteranopia, Protanopia, Tritanopia, High contrast; patterns on/off | Default | live |
| | Magnifier (`magnifier`, `magnifier_zoom`) | Off, Hold key, Toggle key; 2x, 3x | Hold Z, 2x | live |
| Compare | Mode (`compare_mode`, not saved) | Off, Split, Follow cursor, Wipe | Off | live, F8 |
| | Position (`compare_split`) | 0-100% | 50% | live |

* **Auto render scale** is the smallest whole scale whose frame is at least the letterboxed
  output height: `k = clamp(ceil(display_h / 400), 1, 8)`. That gives 2 on a Steam Deck
  (800), 3 at 1080p, 4 at 1440p and 6 at 2160p. The GPU then scales down 0.9x at most, never
  up. A release that ships an `hd` folder (CHANGELOG.md:93-95) then shows HD the first time
  it is started, with no file editing.
* **Presets** set only the rows above, never rules:

| Preset | Look | Scale | Lighting / flicker | HD text / typeface | Motion | CRT | Cursor | Plates / magnifier |
|---|---|---|---|---|---|---|---|---|
| HD (default) | HD | Auto | On / On | On / Remaster | On | Off | 1x | Off / Hold Z |
| Classic 1994 | Classic | Auto | Off | Off | Off | Scanlines 30%, 4:3 aspect | 1x | Off |
| Performance | HD | 3 | Off | On | On | Off | 1x | Off |
| Accessible | HD | Auto | On / **Off** | On / Hyperlegible | On | Off | 2x, outline | On 70% / Hold Z |

### 4.3 Compare slider (classic vs HD)

* **What you see.** A vertical divider across the whole screen, with a knob in the middle and
  "1994" and "HD" chips at the top. On the left are the original pixels, scaled to the HD
  frame with nearest neighbour. On the right is the pack art and the lighting.
* **Where it works.** On every screen, the options page included. The page's own background
  and the title art split live while the slider moves, so the page previews itself. From the
  disk menu, "> Preview" pops back to the game with compare on.
* **Modes.**
  * Split: drag the knob, or nudge it with `[` and `]`.
  * Follow cursor: the divider follows the pointer's x position.
  * Wipe: an animated sweep for trailers and screenshots.
  * Hold F7: peek at Classic while the key is held. A tap toggles, as D2R and SC:R do
    [r1][r2].
* **Rendering.** For each layer, `HdCompositor::draw_layer` draws art commands only to the
  right of `split*k - area.x*k`, using `SDL_SetRenderClipRect`. The base expand (step 1) and
  the base runs (step 2) cover the whole layer, so the left side is exactly the classic
  frame. Lit layers draw `tex` (unlit) to the left and `shown` (lit) to the right
  (`Renderer.cpp:250-270`). While the divider moves, call `redraw_all()`: a fully redrawn 4K
  frame takes about 3.5 ms (HD_REMASTER.md:128-134).
* **Phase 1 limitation.** HD text appears on both sides. The 1994 glyphs are not in the
  indexed layer while HD text is on (CHANGELOG.md:3-9). Phase 2 (P2) renders twice into two
  back textures, once with Look = Classic and once with HD, and splits them at present time.
* **Input.** The knob is the only hit area (`InteractionManager`); every other click passes
  through to the game. The divider is drawn above popups, so `CompareLayer` is pushed last in
  `Overlays`, before `ConsoleLayer` (`Game.cpp:97-108`).

## 5. Recommended roadmap

**Common to every item:**
1. Add a field to `GameOptions` (`GameOptions.h:145-215`).
2. Add an `OptionDef` under a new "HD remaster" section (`ConfigManager.cpp:124-222`).
3. Add a row to `build_remaster()`.
4. Add the option, switched on and off, to the fingerprint gate (`tools/ci/hd_gates.py`).
5. Like lighting, the new rendering options are GPU only. The GPU-vs-CPU picture gate runs
   with defaults (`PLEGACY_CPU_COMPOSE=1` ignores them).

### P0

1. **Look: Classic/HD instant toggle (M).**
   * Add `gfx::hd_art_enabled()` next to `max_hd_scale` (`src/engine/gfx/HdImage.h:67`).
   * Filter the art commands in `HdCompositor.cpp:552-590`.
   * Add `Action::ToggleLook` (`Action.h`, Global section) and bind it to F7 by default
     (`DefaultBindings.cpp:29-37`). Add its label to the `MenuBindings.cpp` action table
     (around `:46-50`). Handle it in `GlobalActionHandler::on_action` (`:32-71`) through a new
     callback (`GlobalActionHandler.h:19`). Wire the callback at `Game.cpp:362-390`: set the
     options, then call `Renderer::redraw_all()`.
   * Classic turns off art, HD text and lighting for display only. The stored HD values are
     kept.
   * Hold-to-peek uses the action's release edge (`InputManager.h:33-34`).
   * Show a "CLASSIC" caption for 2 s in `GameInfoLayer` (`GameInfoLayer.cpp:40-44`).
   * At `hd_scale` 1 the row reads "needs Render scale 2+".
2. **Compare slider (M).**
   * `Renderer::set_compare(std::optional<int>)`, passed through to
     `HdCompositor::draw_layer(…, split)`.
   * Split the lit layers in `Renderer.cpp:250-270`.
   * New `src/views/layers/CompareLayer.{h,cpp}` (divider, knob, chips, drag, keys), pushed in
     `Overlays` (`Game.cpp:97-108`).
   * `Action::ToggleCompare` on F8.
   * Gate: compare on, then the fingerprint must be unchanged.
3. **HD Remaster options page (M).**
   * Section 4: `UIState.h:71`, `MenuOptions.cpp:103-128`, `NavCommands.cpp:45-54`.
   * Auto render scale in `Game::configure_hd` (`Game.cpp:1214-1216`). Change the
     `hd_scale` range to 0-8, with 0 = Auto (`ConfigManager.cpp:146-149`), and make Auto the
     default.
   * Pack picker: scan `<prefpath>/packs/*` and the `hd` folder next to the exe (the turnkey
     path is at `Game.cpp:1225-1231`).
   * Pack coverage: add an `HdPack` count of the pack files that match the export manifest.
   * Presets: an `EnumMenuItem` that writes the rows.
4. **HD cursor and cursor size (S).**
   * Fix hd-audit H1: build an anti-aliased `HdImage` of the arrow at `22*k` and `attach_hd`
     it in `CursorManager`.
   * `cursor_scale` multiplies the destination rectangle in `Renderer.cpp:273-322`.
   * Outline adds a 1-px dark ring, the same pixels in a darker colour. Game Accessibility
     Guidelines ask for "a choice of cursor / crosshair colours / designs" [r22].
5. **Log file (S).**
   * Add a second sink in `Game::run` (`Game.cpp:139`) that writes
     `<prefpath>/logs/plegacy-<timestamp>.log`, keeping the newest 5.
   * Point the "Contributing" bug-report text at it (README.md:175-181). Without it, the
     Windows build has nowhere to show warnings.

### P1

| # | Item (effort) | Wire into |
|---|---|---|
| 1 | Scaling filter + integer scaling (S) | `Game.cpp:544-546`, `923-925` (`SDL_LOGICAL_PRESENTATION_INTEGER_SCALE`); `Game.cpp:1208-1210` (`SDL_SCALEMODE_PIXELART` [r25]) |
| 2 | Frame cap 144/165/240/Display/Unlimited (S) | `FpsOption` (`GameOptions.h:18-86`); `Game.cpp:444-458`; `FpsManager` (`FpsManager.h:13-30`); Display = `SDL_GetCurrentDisplayMode()->refresh_rate` |
| 3 | Alt+Enter / F11 fullscreen, "Pizza Tycoon HD" title, Show FPS (S) | `DefaultBindings.cpp:31`; `Game.cpp:648-652`, `893` |
| 4 | Quick save F5 / quick load F9, "Continue" (S) | New actions (see section 3). Save only when the top UI state is `CityViewState` or `RestaurantViewState` in player mode, with no popup and no `TurnLockLayer` busy. Use `SaveGameManager::save(…, "QUICKSAV.plsave")` (`SaveGameManager.h:59-60`) and confirm before loading. For "Continue", add an item at `MenuMain.cpp:27` that loads the newest save by `read_save_metadata` (`SaveGameManager.h:69-70`) |
| 5 | Message history, Ctrl+H (S) | Popup listing `StatusHub::ephemeral_log()` (`StatusHub.h:129-132`, cap at `StatusHub.cpp:55-57`). The same key as CorsixTH [r14]. View only, nothing saved |
| 6 | Master volume, mute when unfocused, pause when unfocused (S) | `AudioManager.h:157`, `AudioManager.cpp:333-335`; `Game.cpp:797-800`; `PauseLayer.cpp:17-29`. Pause only on a human turn |
| 7 | OPL emulator choice: Nuked, DOSBox, Opal, ESFMu, YMFM (S) | `adl_switchEmulator` after `adl_init` (`AudioManager.cpp:49-58`; `adlmidi.h:710-741`). libADLMIDI can switch at run time [r27]. Audio-tab row `opl_emulator` |
| 8 | Speed widget + 0.5x (S-M) | Make the "Nx" caption clickable (`GameInfoLayer.cpp:30-44`). For 0.5x, make `time_scale` tenths (`GameState.h:325-326,571`; accumulator at `Game.cpp:481-485`). First grep that no core code reads `time_scale()`. AoE II DE has a "Slow" speed [r8] |
| 9 | Hover edge scrolling + scroll speed (S) | `CityView` `scroll_edges_` (`CityView.h:187`) drives `CameraController::edge_active` from the pointer within 8 px of the map edge. Do the same in the restaurant camera |
| 10 | Gamepad pointer, Steam Deck (M) | Left stick moves a synthetic pointer (`PointerTracker`), A = left click, B = right click, through `InteractionManager::process` (`Game.cpp:783`). Right stick = `ScrollVec2` (`Action.h:52`). Speed ±50% (XAG 107 [r20]). Steam Deck Verified requires all content to be reachable with the default controller config [r23] |
| 11 | Hyperlegible typeface + text plates (S + M) | Load a second `data/fonts/hd_fonts_*.json` in `HdTypefaces::configure` (`Game.cpp:1235-1240`). Plates go in `HdCompositor::draw_layer`. XAG 102 recommends text backgrounds with adjustable opacity [r18] |
| 12 | Magnifier (S) | Final frame in `Renderer.cpp:323-328`; `Action::Magnify` held. Stands in for XAG 101's "scale text up to 200%" [r17], which a fixed 1994 layout cannot do |
| 13 | Colour-blind player colours + patterns (M) | `CityView.cpp:993,958,1035,1314-1315`; `CompanySelectionBox.cpp:46-58`. XAG 103: never colour alone [r19]; AoE II DE presets [r8]. Then audit graphs (`GlobalStats`) for colour-only series |
| 14 | Light flicker off (S) | `Renderer.cpp:228-230,257-258` (XAG 117/118 [r21]) |
| 15 | CRT scanlines without a shader (S) | MOD-blend overlay in `Renderer.cpp:323-328`. Only when the output height is at least 1200 (3 texels per 400-line row; DOSBox Staging uses the same "3x" rule [r16]) |
| 16 | Portable mode (S) | `platform_paths::init` (`PlatformPaths.cpp:19-29`): if `portable.txt` sits next to the exe, `base_dir = <exe>/userdata/`. Document in README.md:112-116 |
| 17 | Local crash dumps (S) | `main` (`Main.cpp:232`): `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` into `<prefpath>/crashes/`, flush the log. Never upload. No console window |
| 18 | Music pack + jukebox (M) | `music/<set>/` holds OGG files per `MusicId` (file-formats.md:276-287). Add `stb_vorbis` and an `AudioManager` stream path (`AudioManager.cpp:220-260`). The in-restaurant jukebox keeps its `MusicId` → consumer-type effect, so gameplay is unchanged. This is the C&C and OpenTTD model [r4][r9] |

### P2

* HDR output (`SRGB_LINEAR` [r26]), so light emitters can go past white
  (CHANGELOG.md:20-22).
* Full CRT and colour-blind shaders through `SDL_CreateGPURenderer` /
  `SDL_CreateGPURenderState` [r24][r25].
* Two-pass full-fidelity compare.
* Compare and clean screenshots (no cursor, side-by-side PNG).
* Pillarbox border art.
* Mono audio and output device choice.
* Mouse capture.
* Adaptive VSync.
* Tooltip scale and text colour presets.
* Localization: German Pizza Connection data, plus a string table for the engine's UI.
* In-game manual.
* Screen narration (OS text-to-speech).
* Local achievements that only observe play.
* Opt-in survey with a preview, as OpenTTD does [r10].
* Update check, as CorsixTH does [r13]. It needs a public release feed.
* Mod manager and SDK (sdk-roadmap.md stages 1-7).
* Cloud saves: Steam Auto-Cloud only if the game is ever sold on Steam [r29]. Otherwise portable
  mode lets players point the saves at a synced folder.

## 6. Sources

[r1]: https://www.gameinformer.com/2021/04/09/instant-swap-with-diablo-ii-resurrected-graphics-is-nostalgic-magic
[r2]: https://gameranx.com/features/id/116540/article/starcraft-remastered-how-to-swap-new-old-school-graphics/
[r3]: https://www.mobygames.com/game/41611
[r4]: https://pcgamer.com/the-command-and-conquer-remastered-collection-is-an-excellent-overhaul-of-two-still-great-games
[r5]: https://ea.com/games/command-and-conquer/command-and-conquer-remastered/features
[r6]: https://www.ea.com/games/command-and-conquer/command-and-conquer-remastered/news/steam-workshop-support?isLocalized=true
[r7]: https://techradar.com/gaming/ea-is-releasing-the-source-code-for-command-and-conquer-and-adding-steam-workshop-support-to-further-empower-the-community-to-create-content-for-the-classic-games
[r8]: https://www.ageofempires.com/age-ii-de-accessibility/
[r9]: https://wiki.openttd.org/en/Manual/Game%20options%20window
[r10]: https://www.openttd.org/news/2023/05/14/policy-and-survey
[r11]: https://docs.openrct2.io/en/latest/setup/options.html
[r12]: https://www.openrct2.io/blog/2026/04/openrct2-v0.5.0-released
[r13]: https://www.github.com/CorsixTH/CorsixTH/blob/master/CorsixTH/Lua/dialogs/resizables/options.lua
[r14]: https://www.gamingonlinux.com/2026/07/theme-hospital-game-engine-corsixth-0-70-released-with-ui-scaling-accessibility-improvements-and-more/
[r15]: https://flathub.org/apps/info.rttr.Return-To-The-Roots
[r16]: https://www.dosbox-staging.org/0.83/manual/graphics/rendering/shaders/
[r17]: https://devdocs.xbox.com/gaming/accessibility/xbox-accessibility-guidelines/101
[r18]: https://learn.microsoft.com/en-us/gaming/accessibility/xbox-accessibility-guidelines/102
[r19]: https://learn.microsoft.com/en-gb/gaming/accessibility/xbox-accessibility-guidelines/103
[r20]: https://devdocs.xbox.com/build/game-principles/accessibility/xag-deep-dives/xag-107-input.md
[r21]: https://learn.microsoft.com/en-us/gaming/accessibility/guidelines
[r22]: https://gameaccessibilityguidelines.com/intermediate/
[r23]: https://partner.steamgames.com/doc/steamdeck/compat
[r24]: https://wiki.libsdl.org/SDL3/SDL_CreateGPURenderState
[r25]: https://en.linuxadictos.com/SDL-3.4-strengthens-interoperability-between-the-3D-GPU-API-and-2D-rendering.html
[r26]: https://mankier.com/3/SDL_CreateRendererWithProperties
[r27]: https://git.1bpm.net/libADLMIDI/about/
[r28]: https://github.com/googlefonts/atkinson-hyperlegible
[r29]: https://partner.steamgames.com/doc/features/cloud
[r30]: https://hothardware.com/news/command-and-conquer-remastered-launches

| Ref | What it supports |
|---|---|
| [r1] | Game Informer: D2R's `G` key switches graphics instantly, even mid-battle |
| [r2] | Gameranx: StarCraft Remastered's `F5` swaps graphics instantly; classic or new audio is chosen separately |
| [r3] | MobyGames: Monkey Island SE switches original/enhanced in real time; hint system; orchestral score; autosave |
| [r4] | PC Gamer: C&C Remastered's `Space` to HD sprites; remastered and classic music, jukebox, map editor |
| [r5] | EA: C&C Remastered features (4K, map editor, 7 h of remastered music, bonus gallery) |
| [r6] | EA news: Steam Workshop support (title only, page not read) |
| [r7] | TechRadar (2025): GPL source, Workshop and editors for C&C games |
| [r8] | Age of Empires: AoE II DE accessibility options |
| [r9] | OpenTTD wiki: Game options window |
| [r10] | OpenTTD: privacy-friendly, opt-in survey |
| [r11] | OpenRCT2 docs: options |
| [r12] | OpenRCT2 v0.5.0: plugin engine moved to QuickJS-NG |
| [r13] | CorsixTH `options.lua`: update check, capture mouse, language, autosave, accessibility and hotkey sub-windows |
| [r14] | GamingOnLinux: CorsixTH 0.70 (2x/3x UI scaling, announcer subtitles, screen-shake toggle, message history Ctrl+H, MIDI devices) |
| [r15] | Flathub: Return to the Roots |
| [r16] | DOSBox Staging shaders: `crt-auto` needs at least 3x vertical resolution; `sharp`; integer scaling |
| [r17] | XAG 101: minimum 18 px body text at 1080p on PC; scaling up to 200% |
| [r18] | XAG 102: 4.5:1 contrast, 7:1 in high-contrast mode, text backgrounds |
| [r19] | XAG 103: never use colour alone; let players choose colours |
| [r20] | XAG 107: remap everything, ±50% sensitivity, input parity |
| [r21] | XAG index (101-123) |
| [r22] | Game Accessibility Guidelines, intermediate: game speed, cursor design, contrast, separate volumes, mono |
| [r23] | Steam Deck compatibility: default controller access to all content; smallest font 9 px at 1280x800 (12 px recommended) |
| [r24] | SDL 3.4: `SDL_CreateGPURenderState` |
| [r25] | SDL 3.4: GPU 2D renderer, custom fragment shaders, `SDL_SCALEMODE_PIXELART` |
| [r26] | SDL: renderer output colour space, HDR via `SRGB_LINEAR` on D3D11/D3D12/Metal |
| [r27] | libADLMIDI: Nuked, DOSBox, Opal, Java, ESFMu and YMFM emulators; switching at run time |
| [r28] | Atkinson Hyperlegible: SIL OFL 1.1, by the Braille Institute, for low-vision legibility |
| [r29] | Steamworks: Steam Cloud (title only, page not read) |
| [r30] | HotHardware: C&C Remastered launch and source code release (title only, page not read) |

Pages that could not be read: `gameaccessibilityguidelines.com/full-list/` (blocked),
the en-us `learn.microsoft.com` pages for XAG 101 and 107 (blocked; the devdocs.xbox.com
copies were read instead), and a magicgameworld.com C&C guide (site error). The matrix
cells that rely on [r6], [r29] and [r30] use only those pages' titles.
