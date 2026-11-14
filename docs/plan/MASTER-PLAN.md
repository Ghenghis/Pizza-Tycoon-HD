# Pizza Tycoon HD: master plan (live)

The single ranked plan. It merges five inputs:

- the Codex completion review: `docs/codex-completion-20261114/` in the PC checkout, pinned to `94d616e`, with requirements R01 to R27 and work packages W00 to W10;
- the four gap audits in `docs/gap-analysis/`: contract compliance, HD visual coverage, features against 2026 remasters, and engine performance;
- the blueprint in `docs/blueprint/`;
- the HD audit in `docs/hd-audit.md`;
- the owner's decisions recorded below.

Status words are strict:

- **planned:** not started.
- **in progress:** an owner is changing code.
- **implemented:** the code is merged and its own tests pass.
- **verified:** the acceptance check in the requirement passed on the real target (the Windows PC, or later an Android device), with the evidence linked.

A finished document, build or agent run never counts as verified. Nothing in this file is verified unless an evidence link says so.

## Source lineage and evidence ledger

| Where | Head | Contents |
|---|---|---|
| PC checkout `<work>\repo` (`<work>` is the Pizza-Tycoon-HD-Work folder on the owner's PC) | `94d616e` | HD text v1; what the Codex review read |
| GitHub `main` | `94d616e` | Moves forward only after the PC gates pass |
| GitHub `integration/2026-11-14` | `39c56ee` | Immutable baseline for the AMD worker (tree `ea5e4723`) |
| GitHub `integration/2026-11-14-r6` | `de7ed28` | Every integrated commit through round 6 (tree `641c90a7`) |
| Bundle for the AMD worker | `<work>\bundles\pizza-tycoon-hd-94d616e..39c56ee.bundle` | `94d616e..39c56ee`, sha256 `b0513dc31cb0fb49d429dc2e5b99edb849c01fc070783ed3debb56a2192a61c7`; tree `ea5e47230b4bd9d424f123c12e38dda2a9a5f2a8` |

### Integrated commits (oldest first)

| Commit | What | Lane |
|---|---|---|
| `8b92b4f` | Blueprint docs; HD audit | blueprint agent |
| `15c9a2a` | Glyph sheets, atlas pages, ARGB8888 textures, pooled targets, device reset, upload failures, bench breakdown | integrator |
| `97a253e` | Audio mixing and atomics, PNG budgets, prefetch boundary, manifest containment | platform agent |
| `0507159` | CI gates, scans, docs, Pillow 13, black 26.10 | quality agent |
| `4303862` | Batched quads, smooth cursor, log file, Optimus exports, single version 1.1.0, warnings as errors | integrator |
| `1582b96` | Atomic saves, settings and bindings; bounded and validated loads; the scratch-state commit | persistence agent |
| `1fd2a9e` | Studio location, content and save contract (design only) | architect agent |
| `223f5ef` | F7: 1994 look; F8: 1994 and HD side by side | integrator |
| `9c5b776` | No geometry batching on SDL's software renderer (premultiplied blend bug there) | integrator |
| `39c56ee` | Contract decisions from the owner; references to the unrelated projects removed | integrator |
| `dda5539` | F7 test samples inside the letter | integrator |
| `435be95` | The software cursor follows the real mouse between engine pixels | integrator |
| `589a436` | HD resolution: Auto by default (ceil(display height / 400), 2–8), chosen in the graphics options | integrator |
| `b42fedb` | Classic golden fingerprint tests (contract step 0) | core agent |
| `9da209c` | Content keys, the Classic adapter, per-game roster (contract steps 1–2) | core agent |
| `17ef82a` | Screen-coverage gate, release check, self-hosted runner workflow | quality agent |
| `9b1a1fe` | Classic golden values for linux-gcc-x64: 8 of 9 checkpoints | integrator |
| `30ac96a` | Saves round 2: checked enums, stale temp cleanup, bounded readers | persistence agent |
| `f134e72` | Audio round 2: device loss and change, master volume, mute in background | platform agent; wiring by integrator |
| `720020b` | hdpack tools: file names are data, nothing leaves its folder | platform agent |
| `f16a728` | Saves: every player slot required; no crash on a short list (completes `30ac96a`) | persistence agent |
| `94fec32` | HD far city map (H4), district hatching (M2), newspaper paper restore (N1) | visual agent |
| `2d059ba` | Legacy v1 fixture from a real quick start (17,725 bytes); its golden value recorded | persistence agent; recorded by integrator |
| `de7ed28` | Far-map cache test uses a palette that changes a colour in use | integrator |

### Test and gate results

Clean worktree `integ`; cloud build lane with 2 cores.

| Build | Result |
|---|---|
| Round 1: `15c9a2a` plus the audio patch | 60 cases: 58 passed, 2 skipped; [audio] and [budget] 16 of 16; 0 warnings |
| Round 2: `1fd2a9e` (warnings as errors) | 85 cases: 78 passed, 1 **failed**, 6 skipped. The failure is the tinted-coverage GPU/CPU test (254/255); `9c5b776` fixes it. [audio], [budget] and [save] all passed. The Windows build was stopped, as `39c56ee` replaces it. |
| Round 3: `39c56ee` | 86 cases: 79 passed, 1 failed (a sampling mistake in the new F7 test; fixed in `dda5539`, where it passes), 6 skipped; [audio], [budget] and [save]: 37 passed, 4 skipped; 0 warnings. Windows exe sha256 `0f2a8022...a757a` staged as `<work>\play-39c56ee` |
| Round 4: `589a436` | 86 cases: 80 passed, 0 failed, 6 skipped (they need the original game). The Windows build was stopped: round 5 replaces it. |
| Golden recording at `b42fedb`, with the original game | 3 of 4 golden tests recorded 8 checkpoints (`9b1a1fe`). **Failed: the v1 save fixture crashes on load** (SIGSEGV in `GameState::human_player_count`, an empty player slot, called from `TurnOrchestrator::run_city_start_of_day` inside `SaveGameManager::json_to_state`). The [save] test that loads the same fixture crashes the same way; 23 of 24 [save] cases pass, the round trip included. A hostile save can reach this, so it is a robustness bug, not only a fixture problem. Sent to the persistence lane. |
| Round 5: `720020b` | No crash any more: the v1 fixture is now refused cleanly. 117 cases: 103 passed, **3 failed**, 11 skipped; with the game, 115: 111 passed, **3 failed**, 1 skipped. All failures are save tests built on the old 2-player game: `30ac96a` had picked up the stricter validator from the working tree without its tests (`f16a728` completes it). Windows exe built; 0 warnings. |
| Round 6: `94fec32` | With the game, after the recorder wrote the new fixture: 126 cases, 123 passed, 1 skipped, **2 failed**. The failures were the v1 golden checkpoint, not yet recorded (recorded in the same run as `84c8e5a930faab92`), and a far-map cache test bug (fixed in `de7ed28`). **The 8 Classic golden values from `b42fedb` are unchanged** with the roster, saves, audio and HD changes in. Windows exe built; 0 warnings. |
| PC: `PizzaTycoonHD-text-9.exe` (glyph sheets, before the ARGB, base-texture and batching changes) | Steady avg 3.3–4.3 ms, p99 5.0–5.7 ms. First frame **16.8–17.4 ms: FAILS** the 16 ms gate. HD text off: 10.7–11.8 ms. |
| PC bench gate r2 | Ready, not run: `<work>\bench-gate-r2.ps1` sha256 `d0d1ba09...0a0ad`. It answers the coordinator's review of v1 (`msg_0b98b89aa85d2bfc`). Content pins: exe `0f2a8022`, data `a5fe3281`, pack `a9d3a378`, game `a4d02cd1`. Broker check before each launch. Handle-only cleanup, one trial deadline, receipt written in finally. Parser and PSScriptAnalyzer clean; negative path checked on the PC (FAIL in 4.6 s; nothing started). Sent as `msg_f6d09620548a30c2`. |
| PC gates for round 3 (v1, superseded by r2) | **Waiting for a grant.** The broker denied it at rev 16/17 while the freezes were diagnosed. Readiness was sent to codex-einhander-crash-check as `msg_a8c4f3539628225c`, with the one command, all identities and the cleanup bounds (`<work>\logs\readiness-39c56ee.txt`). `bench-gate.ps1` sha256 is `ee527b47...dc97a`, parse-checked on the PC. |

### Lanes and their current tasks

| Lane | Status | Current task |
|---|---|---|
| Integrator (build lane) | active | PC bench gate r2 when granted; then the play folder (with rollback), `main`, coverage budgets and the Windows golden values; integrate each lane's report |
| Core/catalog | active | Contract steps 3–6 and the scenario block: definition-backed city source, goal observer, Santa's Pizza Village and the Cozy Village scenario, a headless journey test (day, week, save, restart); null checks in `human_player_count` and the competition loop |
| Persistence | active | Three verified working versions per save slot, pre-change checkpoints, fault injection (COMMANDS-AND-RECOVERY step 2); duplicate command names rejected |
| Visual | active | Far map built off the frame; M6 medium map; M4 water cycling at close zoom |
| Platform/audio (the one R35 owner) | active | One music-mode owner (Original, Christmas Background, Jukebox, Off) with a playback generation; dr_mp3 and dr_wav; Xmas and chosen folders, collections, playlists, settings with three last-good revisions; libVLC 3 adapter loaded at runtime into the game mixer. The mini-player, the menus and the JUKEBOX 138 hook come next. |
| Quality/release | done (`17ef82a`) | Next: record the coverage budgets on the PC run; release check on the next exe |
| AMD worker | handoff received by its coordinator | Proposed independent tasks: native Windows build and Windows golden values; RX 7800 XT renderer qualification; AMF probe for R34 |

## Owner decisions (fixed inputs)

- Native Windows first. Native Android (Samsung S21 and the tablet) is a feasibility target with its own device evidence. macOS is optional and is not Android.
- Classic mode is the exact 1994 game. Expanded content has its own identity and versioned saves. No 2.5D rewrite.
- The Studio has three ways to create, all sharing one editable document:
  - **Quick Create** is fully automatic.
  - **Guided Create** walks the user through the steps.
  - **Full Editor** gives direct editing.

  Studio features:
  - locks on manual edits;
  - selective regeneration;
  - cancel and resume;
  - verification of structure and of gameplay, within set bounds;
  - published locations reachable with previous and next controls.

  Generated art alone is never a playable map.
- **World navigation:**
  - 8 regional overview maps cover the world, each with searchable city pages.
  - Each overview shows 8 to 12 location markers.
  - List pages hold 12 locations on desktop and tablet, or 6 on a phone, and grow as users create locations.
  - Browsing never advances time and never travels silently.
- **Commands and recovery:**
  - start with 500 to 1,000 useful commands, each implemented, tested and unique, with no cap on the catalog later;
  - a typed, versioned API, with aliases kept separate from commands;
  - the last 3 verified working catalog versions kept;
  - staged updates with rollback;
  - timed and named checkpoints in a Recovery Center;
  - undo and redo, restore as a branch, and undo of a restore;
  - safe experiments;
  - portable backups, verified optional copies off the device, and retention budgets;
  - no padding the command count.
- **Dependencies:** audit them against official sources. Upgrade one group at a time, with measurements and rollback. No blanket upgrades, and no required cloud service.
- **GPU:** the 3090 Ti follows the shared-GPU inbox protocol. It has a user hold until at least 18:37 UTC on 2026-11-14; after that, request a fresh grant.

## Ranked plan

The reliability work and the first playable slice come before broad command expansion.

| # | Work | Req | Owner (lane) | Status |
|---|---|---|---|---|
| 1 | **W00 Windows render/perf delivery.** Text atlas pages, pooled render targets, font prewarm at startup, glyph sheets (one tinted GPU quad per letter), first-frame breakdown in the bench. Gates on the PC, then the play folder (with rollback), then push. | R01 R14 R26 | integrator | in progress |
| 2 | **W01 persistence:** checked atomic save, settings and bindings writes; bounded save load into a temporary state, committed only when valid; mission validation; fault-injection tests | R02 R11 | persistence agent | in progress |
| 3 | **Audio and imports:** additive mixing with clipping, atomic gain state, no blocking in the callback, PNG budgets before decode, prefetch exception boundary, manifest path containment | R03 R13 | platform agent | in progress |
| 4 | **Upload failures and render-device reset:** a failed `SDL_UpdateTexture` must not mark a texture ready; recreate resources on `SDL_EVENT_RENDER_DEVICE_RESET` / `TARGETS_RESET` | R26 | integrator | implemented (not yet tested by a real reset) |
| 4b | **Perf audit quick wins:** native ARGB8888 textures (no conversion copy per upload); one base texture for every layer size; the atlas flush sends the dirty rect only; target pool of 12; letters move closer before being narrowed, so more lines use the glyph sheet. Next: batch letters with `SDL_RenderGeometry`, skip empty overlays, fade by colour mod at composite time, a frame cap that matches the refresh rate, a cold bench with a GPU sync, the `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exports, LTO, then PGO, all measured | R14 R26 | integrator | in progress |
| 4c | **Back-texture double blend:** the perf audit suspects that soft-alpha upper layers get blended again when only they change. Confirm or rule this out with a test before changing the compositing order | R26 | integrator | planned |
| 5 | **CI that cannot pass by accident:** doctor exit code, skipped gates counted, gate artifacts kept, plus secret, dependency, forbidden-pattern, bandit and mypy gates; real warnings-as-errors | R21 | quality agent | in progress |
| 6 | **Single version source, tags, release gate** (exe plus zip plus checksums; the bare exe must not depend on a missing `data/`) | R20 | quality agent, then integrator | in progress |
| 7 | **Docs match the code:** 20 stale claims; SECURITY risky surfaces; ARCHITECTURE covers the GPU draw-list path | R21 | quality agent | in progress |
| 8 | **W02 location/content/save contract:** inventory of fixed-city uses, stable IDs, LocationDefinition schema, save contract, regions and paging, fixtures | R04 R05 R08 R11 | architect agent | in progress |
| 9 | **Classic/HD instant toggle and compare slider** (F7/F8, `223f5ef`), plus HD settings in the menus (Auto scale, pack picker, presets) | R19 | integrator | toggle and compare implemented (not yet verified on the PC); options page planned |
| 10 | **HD visual gaps, top tier:** HD cursor with sub-pixel motion (H1, N3); city far map (H4); portraits (H3); newspaper paper (N1); feathered sprite edges (H2); hatching and diagonal primitives (M2, M3) drawn with tinted HD commands | R12 R27 | integrator plus art lane | planned |
| 11 | **Log file** (`4303862`), crash dumps, portable mode; mute on focus loss and master volume | R17 R26 | integrator / platform agent | log file implemented; the rest assigned |
| 12 | **W03 first playable generated location:** one trading day and a fresh-process reload | R05 R06 R11 | core agent (after #8) | planned |
| 13 | **W04 native Studio entry:** Quick Create, browser with previous and next, one command and undo model | R07 R08 R09 | Studio agent (after #8) | planned |
| 14 | **W05 input parity:** touch commit and cancel semantics, gamepad pointer | R10 | platform agent | planned |
| 15 | **W10 command platform and Recovery Center** (after #12 and #13 are stable) | R23 R24 R25 | core agent | planned |
| 16 | **W07 Android feasibility slice** (NDK arm64, lifecycle, memory budget, real S21 and tablet) | R15 R16 | Android agent | planned |
| 17 | **Art pipeline truthfulness:** separate counts for present, valid, authored, reviewed and live; prompt subjects fixed (N7); sheet cells upscaled separately (L7) | R12 | art lane (GPU after the hold) | planned |
| 18 | **W08 and W09:** integrated acceptance; release, upgrade and rollback | R18 R20 | acceptance and release | planned |

## Lanes and rules

- **Build lane:** one 2-core lane in the cloud, run by the integrator. Agents write patches only to the files they own, check their own files with `-fsyntax-only` at low priority, and hand the patch back. The integrator then builds, runs the tests and fixes what breaks.
- **File ownership** for the current round:
  - integrator: renderer, compositor, text, `CMakeLists.txt`, `source_manifest.cmake`, CHANGELOG;
  - persistence agent: `src/core/save`, config and bindings writes, `AtomicFile`;
  - platform agent: audio, `Png`, `HdPack`, `hdpack.py`;
  - quality agent: scripts, `tools/ci`, README, SECURITY, `HD_REMASTER.md`, CREDITS;
  - architect agent: `docs/studio`, `tests/fixtures/locations`.
- **The PC:** everything runs hidden; never use the D: drive; both GPUs never run at full load together; the shared-GPU protocol applies to the 3090 Ti.
- **Evidence:** every result carries the source head, the exe name, the pack, the command and its raw output. Failed and skipped checks are kept, not deleted.

## Content scope (updated 2026-11-14 11:41 Phoenix)

Source: the user's message plus five new Codex docs in the PC checkout. The manifest now covers 16 files and 33 open requirements, and its source review is still pinned to `94d616e`.

The new docs are `WORLD-REGIONS-AND-PAGINATION.md`, `REGIONAL-ART-STORIES-AND-SEASONS.md`, `MAIN-MENU-WORLD-DESIGN.md`, `NORTH-POLE-SPECIAL.md` and `HOLIDAY-CAST-ASSETS.md`.

**Locations.**
- There are 8 canonical regions, each with 3 distinct starter areas: 24 new locations.
- The fictional bonus destination **Santa's Pizza Village** (North Pole) makes 25 new locations, or 35 together with the 10 Classic cities.
- That total counts **only once each location has passed gameplay and save acceptance**.
- The product name stays **Pizza Tycoon HD**.

**North Pole special.**
- It is playable all year, always.
- It has three presets built only from economy features that already exist: Cozy Village, Workshop Rush and Pizza Empire. Sandbox play continues after the goal.
- Christmas decoration is optional and never locks the area to a date.

**Cast.** Santa, Mrs. Claus, Rudolph, Frosty, an elf chef, an elf courier, a gingerbread baker, a nutcracker host, four holiday staff outfits, and all nine reindeer. That is 20 concept entries. Each still needs complete frames, alpha, role bindings and save references before it can be used in the game.

**Studio library.**
- Locations are tagged by country, state, community, biome and season.
- Each region keeps its own architecture through per-region kits.
- Areas made by users stay editable.

**Calendar events.**
- Modes: off, in-game calendar, or real date. Real date is opt-in, with a manual preview.
- Reduced motion is available.
- Events never touch the economy.

**Title and main menu.**
- The world backdrop is built from game art that has passed admission.
- The menu keeps Continue, New, Load, Studio, High Scores, Options and Exit, with accessibility and the first-open gates.

### Asset inventory

Read from `<work>\world-region-art\ASSET-INDEX.json`. Its own status line reads: "candidate asset library; no runtime integration or gameplay acceptance".

| Set | Count | State |
|---|---|---|
| Location scene candidates (batch `20261114T183013Z`, juggernautXL_v9) | 24 at 1536x960 native, plus 24 explicit 4K resizes | Generic modern commercial references. They need local architecture and signage curation. **Not** region-authentic art and **not** playable maps. |
| Geographic overview backgrounds (Natural Earth, equirectangular, no political borders) | 9 PNG + 9 SVG (world + 8 regions) | Authoritative for coastlines. Source, licence and hashes are kept. |
| Menu concept v1 | 1 static JPEG plus provenance; uses the existing `titel1.png` | A layout concept, not a working menu. |
| North Pole preview | `north-pole-preview.jpg`, master receipt `183818Z` complete | Candidate. |
| First regional atlas attempt `20261114T182320Z` | — | Not selected. The style failed (it came out medieval). |
| North America pilot `20261114T182619Z` | — | Scene reference only. |
| Holiday cast concepts (`holiday-cast-catalog-v1.json`) | 20, generating one after another since `183935Z` | Opaque backgrounds. They are concepts, not sprite sheets. |

**Still to do, per the index:**
- per-image art, reference and signage review;
- runtime tile, sprite, facade, character and costume production;
- 24 playable definitions and scenarios;
- Studio and menu integration;
- real gameplay, save, performance and platform acceptance.

### Content gates (in order; nothing skips a gate)

1. **Contract.** `docs/studio/` is frozen: LocationDefinition, scenario and preset schemas, the asset admission record, facets, the event catalog, and cast and costume IDs. Owner: the architect agent.
2. **Reliability.** The save and load hardening (W01) and the audio and import budgets (R03, R13) are merged and pass their negative tests.
3. **One playable slice.** One generated location from a 160x120 template is played through acquire, furnish, menu, stock and staff, service, one trading day, save, and a reload in a fresh process. Proposed first candidate: Santa's Pizza Village on the Cozy Village preset, because it is year-round and the user's favourite. Owner: the core agent, after gate 1.
4. **Asset review.** Every candidate gets a recorded decision (curated, style-reviewed, runtime-validated, admitted) with its provenance and hashes. A contact sheet alone doesn't pass this gate. Owner: an art-curator lane, review only. It needs no GPU.
5. **Breadth.** Only after gates 3 and 4: the other 24 areas, the cast frames, the world menu, and then command and recovery expansion (W10).

### Machines and GPU

- **3090 Ti:** under the shared-GPU broker. Rev 17 at 20:2x UTC: no grant, and this project is on the denied list while the Tapper preservation copies hold the main host's I/O. **No GPU work, no game runs and no ComfyUI batch from this lane until a fresh grant.** Before each batch: `check` with the token, then `start` with the exact PID and creation time, then an explicit `release` after verified cleanup.
- **AMD PC:** its coordinator has the handoff (`39c56ee` verified on GitHub). It uses its own resource envelope, not the broker.

### Recording and private music (R34, R35)

Source: `RECORDING-AND-MUSIC.md`. These come after the reliability work and the playable slice.

- **R35 private music library** (refined by `NORTH-POLE-MUSIC-LIBRARY.md`, 13:18 Phoenix):
  - Modes: Original, Christmas Background, Jukebox and Off. Choosing one turns the other Christmas mode off. One owner, one final game mix for volume and recording.
  - An `Xmas` folder beside the exe, or a chosen folder. Each subfolder is a named collection. Playlists can be edited inside North Pole.
  - A compact Winamp-style player: title, transport, seek and volume, with More for the advanced settings. The setup is saved.
  - Engines: the owner's proven player (Ghenghis/Fling-Studio at `fa1e6212`: LibVLCSharp 3.10.2 over native VideoLAN 3.0.24) is reused as native libVLC 3, through its decoded-audio callbacks, behind one adapter. dr_mp3 is the portable direct path; Android does not use MediaPlayer, which bypasses the mixer.
  - The North Pole jukebox is the existing furniture `JUKEBOX` (138) and `JukeboxPopup`. Its 0–6 `jukebox_music` stays gameplay data (saved; +12/−4 appeal). Private music state stays separate.
  - The owner's library: 7 collections, 402 MP3 files (about 2.98 GB). Play them directly; convert only what fails.
- **Asset reuse** (`ASSET-REUSE-POLICY.md`, latest): compatible owned art and audio from Tapper, PC3, Barro's Pizza, Kenney, CraftPix and other backups may be reused for either game after review. This supersedes the earlier art exclusion. Code and schemas still need their own compatibility review, and project identities stay distinct.
- **R34 game-only recording.**
  - Explicit start and stop with an indicator; 60 minutes by default.
  - A codec is offered only after it has actually been opened on that machine. NVENC H.264/HEVC on Ampere; no AV1 there. AMF on the RX 7800 XT once probed.
  - Recoverable segments, a byte quota and a free-space reserve.
  - The official FFmpeg/SDK setup is approved by the owner.
- **Owner's lane: platform/audio, design first** (`docs/studio/notes/music-library.md`). Recording follows the playable slice.
