Unreleased

* HD lighting (option `lighting`, on by default; also in the options menu):
  candles, ovens and grills, arcade machines, TVs, the jukebox and vending
  machines light the restaurant around them, with flicker (flames, fire,
  screens, the jukebox's slowly turning colours), and the room follows the
  game clock: bright by day, warm in the evening, dim and blue at night.
  Shader-free GPU pass (`LightPass`): a light map at engine resolution,
  multiplied in and doubled, so lights can be brighter than the art, plus an
  additive halo on emitters. Only the picture changes; GPU-only by design.
* `--exec` prints what each command reports, so scripts can read results.

Pizza Tycoon HD 1.1.0 (2026-11-14)

* End-to-end gates (`tools/ci/hd_gates.py`, `scripts/hd-gates.ps1`, part of
  both local CI scripts): the real game, run hidden, must draw the same
  picture with GPU and CPU compositing, keep the full game state identical
  with smooth motion on and off, and on a real GPU keep a fully redrawn 4K
  restaurant frame under 8 ms (99th percentile) and its opening under
  16 ms. On the reference PC: 1/255, identical, 4.4 ms and 14.4 ms.
* GPU compositing for HD: surfaces keep their HD art as a draw list
  (`gfx::HdList`) instead of copying hd_scale()^2 texels per pixel on the
  CPU, and `HdCompositor` draws the list with the GPU through any SDL
  renderer. Measured on a Ryzen 7 5800X3D with Direct3D 11, a fully redrawn
  3840x2400 restaurant frame: 3.5 ms (285 fps) instead of 80 ms (12 fps);
  the picture matches the CPU path within 2/255 per channel.
  `PLEGACY_CPU_COMPOSE=1` keeps the CPU path as a fallback.
* Fix: semi-transparent HD texels were blended twice on all but the first
  texel row of each pixel, so soft sprite edges came out too strong.
* Smooth motion (option `smooth_motion`, on by default; also in the options
  menu): walking guests and waiters glide between steps at the screen's
  frame rate instead of jumping 12 pixels four times a second. Only the
  drawing changes: the full game-state fingerprint after 2,000 ticks is
  byte-for-byte the same with it on and off.
* Fix: `GameState::game_mode_` was left uninitialized until a game was set
  up, which made state fingerprints differ between identical runs.
* `hdpack.py upscale --method mmpx`: pixel-art magnification (MMPX, the
  engine's own rules, vectorised in `tools/hdpack/pixelart.py` and checked
  against a line-by-line copy of the C++) to 16x, then an alpha-correct
  area average to the target. All 1,554 sprite frames take about 20 s at
  12x; edges stay crisp and no colour is invented, where AI repainting
  turned small sprites to mush.
* No pause when a screen first opens: the HD pack remembers which pictures a
  session used (`hd_warm.txt` next to the config), decodes them on a
  background thread while the next session's intro plays, and the renderer
  streams them to the GPU a few milliseconds per frame. Opening the
  restaurant at 3840x2400 on a 5800X3D + RTX 3090 Ti: first frame 222 ms
  before, 55 ms with decoding moved off the frame, and streaming removes
  most of the remaining upload. Checked with ThreadSanitizer.
* `tools/hdpack/depthmaps.py`: depth (Depth Anything V2) and normal maps for
  every pack picture, the inputs for 2.5D lighting.
* `bench [frames]` console command: frame times of the current screen,
  worst case (everything redrawn every frame).
* `--exec` batch runs keep their window hidden.
* `hdpack.py comfy`: pictures under 4096 original pixels (`--small-area`)
  skip SDXL and use model upscaling only (`--small-workflow`); a first GPU
  test showed SDXL turns tiny sprites and portrait sheets to mush.
* Both shipped workflows end with a model upscale before the final resize,
  so `--scale 12` (3840x2400, 4K) stays sharp instead of being stretched
  from the ~1 megapixel diffusion output.

Pizza Tycoon HD 1.0.0 (2026-11-13)

Based on Pizza Legacy 1.0.0-beta.2.

* HD rendering: `hd_scale` (1-8) / `--hd-scale` draws the 640x400 game screen
  at that many texels per pixel; the GPU scales the frame to the window.
* HD art packs: `hd_pack` / `--hd-pack`, or an `hd` folder next to the
  program. Any VGA picture or BOB frame can be replaced by a PNG of the same
  aspect ratio and any size; missing files keep the original art.
* `--export-assets <dir>` writes all 145 VGA pictures and 1,554 BOB frames
  with the palettes the engine uses, plus `manifest.json`.
* `tools/hdpack`: baseline upscaler, pack validator, ComfyUI batch generator
  with two workflows and per-asset prompt rules.
* New 4K art: box cover (`assets/art/`) and title screen
  (`assets/hd/gfx/titel1.png`, replaces GFX/TITEL1.VGA); releases ship it as
  the `hd` folder next to the program.
* `tools/hdpack/export.py`: exports the original art without the engine
  (same PNGs and manifest, verified pixel for pixel), for the GPU machine.
  `hdpack.py comfy` gained `--denoise` and `--wait-gpu`/`--wait-below`, which
  pause before each picture while another GPU is busy.
* The VGA and BOB viewers open files from the game folder under the game's
  own asset names, so they show the right palette and HD pack art.
* Tests: Catch2 suite linking the real engine (HD image maths, surface overlay
  rules, renderer expansion, pack loading and full export against the
  original files), pytest suite for the pack tools, HD frame benchmark.
* Windows: the game is a GUI program (no console window opens next to it)
  and borrows the parent terminal for output when started from one.
  `scripts/build-windows-cross.sh` builds a single self-contained
  `Pizza Legacy.exe` (SDL3, zlib, libADLMIDI and the C++ runtime linked in;
  only Windows system DLLs imported) from Linux or WSL2, plus the release zip
  and SHA-256 files. Verified under Wine: export and HD renders are
  pixel-identical to the Linux build.
* `PLEGACY_ADLMIDI_STATIC` CMake option links the vendored libADLMIDI
  statically.
* Fixes: the `gfx/euromap` key no longer resolves to `gfx/gfx/euromap.vga`;
  `ArgSpec.h` includes `<cstdint>` (MinGW build); saving a screenshot no
  longer throws when its folder can't be checked or created; two unused
  variables removed (zero compiler warnings).

1.0.0-beta.2 (2026-09-13)

Lots of fixes, some new features; did a lot of work to compare the original
behavior to our version and adjust to make things match better. Notably, mafia
and police are a lot more likely to contact you now, and we use the original
game's calendar now (365 days, no leap years). Also demand should now match the
original which results in more business.

* Closer to the original
  * Mafia and police will now call you on the original's schedule
    instead of a fixed ten-minute check, which makes both much more
    frequent. Mafia mission risk depends on the difficulty and on what the job
    is: fetching a bucket of water is safer than fetching plutonium, and the
    shadier the investment deal, the more likely it falls through. Protection
    timers, shakedowns and account numbers all match now.
  * Demand is derived from the day's actual orders, and real pizza quality,
    furniture popularity and advertising feed into it.
  * Calendar of 365-day years, the original week counter, and the original
    holiday maximum (28 days max instead of our 30). If you had a save game
    that was past the 28th of February 1996 already your save moves one day
    backwards.
  * Computer players are set up on first entry to a city, and they now increase
    in quality, expand and now have scores and awards.
  * You can now get tips from the police about upcoming mayor visits or police
    inspections of your depots.
  * Interest rates are now per city, bank investments paying a twelfth of their
    annual rate, corrected weapon market prices, and a long list of payments
    that were charged wrongly.
  * Newspaper event ranges, values and targets; ingredient trends now flip on
    a coin toss like the original.
  * We now charge the time cost of visiting various screens.
  * Fixes related to chair count vs seat count, and table/chair linking.
  * Cars spawn on the roads the way the original spawns them.
  * A whole bunch of fixes to make screens render closer to the original.

* New
  * Save format v2: we store a small block of metadata ahead of the rest of
    the save game, so that we can render the load game without having to load
    the entire save game (which could be very slow on larger saves). Older
    saves still work.
  * Fixes that affect balancing are now explicit options (config file only for
    the moment): the win condition can be `paris` (the original's behavior),
    `any_city` (what I think the original meant to do, our default) or
    `all_cities` (how the German Pizza Connection worked), and the mafia
    shakedown bug that only ever looks at your first restaurant can be turned
    off.
  * Key repeat, with `key_repeat_delay_ms` and `key_repeat_interval_ms`.
  * A fast-forward time scale, on the console or a key you bind yourself (in
    bindings.conf only for now).
  * We now have a "busy" cursor, and a status layer that shows the time scale
    and the autosave message.
  * The load game screen keeps the autosave slots out of the way behind a "Show
    auto saves" checkbox, and the name/date sort is now a checkbox.
  * On days that no humans can take actions (due to being in prison or on
    holiday) we skip the autosave.
  * Fullscreen is an option in the graphics section as well as Ctrl+F.
  * In the menu we now show a line of help text to give more context on some of
    the menu options.
  * settings.conf now documents every option and its default in comments, so
    the ones with no entry in the options screen can be found and edited. Only
    the options you change are saved.
  * The game now asks for the original's folder on a screen that explains what
    it wants, with Browse and Exit buttons, rather than with a system alert
    dialog.
  * Furniture, ingredients, trainings and suppliers are read from data/*.json
    instead of being compiled in, so the tables can be edited without a build.

* Fixes
  * Crash when reading a pizza's name while it was being moved (#8, reported
    by @StephenFluin).
  * Enter being swallowed while typing (#9, reported and diagnosed by
    @StephenFluin).
  * Debug help output no longer drifts from the commands it lists (#11,
    reported by @StephenFluin), and the libADLMIDI build steps are spelled
    out (#10).
  * Missing raspberries in the pizza design ingredient list (#13, reported by
    @StephenFluin).
  * The hiring pool not updating while the daily report screens were shown,
    and AI economies freezing as a side effect (#14).
  * Milestone messages appearing more than once, and scoring more than once
    with them (#15, reported by @andrewbrereton).
  * Missing DLLs in Windows distribution (#19).
  * Crashes on backspacing over a multibyte character, on an invalid active
    city, and in the pizza competition.
  * Plenty of state that was missing from savegames: score and awards,
    popularity, bank accounts, staff, depots, menu entry quality, a pizza's
    imitation score and the RNG state.
  * Guests walk out of the restaurant instead of vanishing at the doorway.
  * City names sometimes came out in German (from original data files).
  * End-of-turn reports no longer flash the city view, staff events stay
    inside business hours, and clicks are ignored while the turn runs.
  * The monthly invoice didn't mention the investment. Labels are now also
    correct (if the fix legacy bugs setting is on).
  * Restaurants no longer show their popularity score before opening.
  * Some of the status messages (manager went shopping, food spoiled) didn't
    show up.
  * The Automatic Report option now shows the daily settlement screen when the
    option is ON instead of OFF (matching the original).
  * A perfect delivery week now fires on the day the streak completes instead
    of waiting for the end of that week.

* Performance
  * The cursor only redraws its own rectangle, and the city view caches its
    map and tabs, so both cost much less per frame.
  * The game now actually reaches 120 fps when asked for it instead of topping
    out around 115.

1.0.0-beta.1 (2026-07-11)

First feature-complete release: all gameplay systems from the original game
are now implemented.

* Add final gameplay systems
  * Mission mode
  * Newspaper advertising mini-game
  * Staff training: course selection and completion effects on stats
  * Restaurant attacks: enter a competitor's restaurant outside business hours
    with a weapon and cause damage; human defenders can fight back with
    WASD/Left Ctrl or gamepad
  * Walking guests: guests now visibly enter and leave the restaurant, and the
    waiter brings them their pizza
  * Cookbook reminder when recreating a standard recipe scores below 10%
    imitation, like in the original

* UI and options
  * Keyboard navigation of menus: arrows to move, tab/shift+tab to skip over
    widgets with their own items
  * Options screen split into categories (audio, graphics, controls, gameplay)
  * Volume sliders in the audio menu
  * FPS setting (30/60/120) and VSync setting
  * Software cursor (default): the cursor is drawn by the game renderer so it
    can no longer outpace the game, fixing lagging placement of ingredients in
    Pizza Design during fast mouse movement; hardware cursor remains available
    as an option
  * Gamepad support and key bindings on modifier keys

* Fixes
  * Job market: fired staff can no longer be immediately rehired
  * First mafia mission no longer always fails, and a wrongly scoped mafia
    milestone was corrected
  * Restaurant/depot closure now has its missing social score impact
  * Conversations no longer push time past the end of day
  * Standard pizza judging bug fixed
  * Capacity screen now uses today's forecast like the production screen,
    matching the original
  * Cursor aspect ratio in fullscreen on Windows (thanks to Reddit user
    plonspfetew for the report)

* Internals
  * Migrated from SDL2 to SDL3

0.1.0 (2026-05-25)

* Add missing screens
  * Daily profits
  * Cash flow/liquidity
  * Restaurant sales curve
  * Restaurant sales figures
  * Customer tastes
  * Property overview

* Add interactions
  * Mafia with missions
  * Mayor with awards and restaurant stars
  * Police with arrests
  * Insurance with buying and claiming
  * Bank investments and money transfers
  * Staff random events (lateness, pregnancy, theft, mistakes)
  * Pizza competitions (end of month/end of year)

* Add game systems
  * Fullscreen status messages and win/lose scenarios
  * Supplier delivery contracts
  * Job market: character availability scaled by difficulty; expand to max 200
    characters via the original's character doubling system
  * Manager role: handles staff events, restocks ingredients, adjusts menu
    prices autonomously
  * Sabotage: joke articles (smelly cheese, laxatives, rats, stink bombs)
  * Autosave

* Various UI changes
  * Load game screen: list sorting, show last modified date in list
  * Mouse wheel support expanded: city map zoom, staff candidate cards, ingredient/furniture
    order counts, furniture category switching, number input popup
  * Right-click map scrolling in city and restaurant views (grab-and-drag and inverse)
  * Pizza menu prices now editable after adding to menu
  * Status messages behave closer to original

* Various game changes
  * Operations now cost time (placing furniture, calling people)
  * Fix bugs (from original) in ingredient related newspaper events
  * Food supplier sort order corrected to match original; cut suppliers removed from list

* Internals
  * New, smaller, savegame format (not compatible with v0.0.1 saves)
  * Audio engine optimizations and initialization moved to intro screen
  * Rewrote end-of-turn system completely for better code clarity
  * Steam and GOG app folder auto-detection when configuring the PT.EXE path


0.0.1 (2026-03-07)

* Initial release
