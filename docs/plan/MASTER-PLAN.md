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

## Source lineage

| Where | Head | Contents |
|---|---|---|
| PC checkout `C:\Users\Admin\Pizza-Tycoon-HD-Work\repo` | `94d616e` | HD text v1. This is what the Codex review read. |
| Cloud checkout (integrator) | `8b92b4f` plus uncommitted work | `8b92b4f`: blueprint and HD audit. Uncommitted: render and perf work W00 (see below). |
| GitHub `Ghenghis/Pizza-Tycoon-HD` main | `94d616e` | `8b92b4f` is not pushed yet. It goes up with W00, once the gates pass. |

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
| 9 | **Classic/HD instant toggle and compare slider**, plus HD settings in the menus (Auto scale, pack picker, presets) | R19 | integrator | planned |
| 10 | **HD visual gaps, top tier:** HD cursor with sub-pixel motion (H1, N3); city far map (H4); portraits (H3); newspaper paper (N1); feathered sprite edges (H2); hatching and diagonal primitives (M2, M3) drawn with tinted HD commands | R12 R27 | integrator plus art lane | planned |
| 11 | **Log file, crash dump, portable mode;** audio focus mute and master volume | R17 R26 | platform agent (next) | planned |
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

Read from `C:\Users\Admin\Pizza-Tycoon-HD-Work\world-region-art\ASSET-INDEX.json`. Its own status line reads: "candidate asset library; no runtime integration or gameplay acceptance".

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

- **3090 Ti:** under the shared-GPU broker. Codex holds grant rev13 until 18:50 UTC and will stop, unload and release the card explicitly. Until it does: **no GPU work, no game runs and no ComfyUI batch from this lane.** This includes the PC gate runs, because the game itself uses the GPU. After that, check the broker and request a fresh grant before each batch.
- **AMD PC:** offline over Chrome Remote Desktop since 11:03 Phoenix. It is not counted as a live worker.
