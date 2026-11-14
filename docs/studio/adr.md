# W02 architecture decisions

Decisions behind `location-contract.md` (contract 1.0.0-review). They build on the Codex review's
ADR-01..04 (`SOURCE-REVIEW-AND-ARCHITECTURE.md`) and do not change them. Nothing here is
implemented. File references are to this checkout.

## W02-01 Dynamic location count through an immutable scenario roster

- DECISION: Every game has an ordered roster of location keys fixed at creation and saved; roster
  position is the runtime `CityId`. The Classic roster is the original ten in today's order.
- BECAUSE: The fixed arrays (`GameState.h:235,545`, `TurnOrchestrator.h:65-67`, `Player.h:279`,
  `PlayerFinancials.h:111-112`, `WeaponMarket.h:47-48`) become vectors with no change in Classic
  values, and RNG draw counts that depend on the city count (`WeaponMarket.cpp:93-115`,
  `MafiaConversation.cpp:351`) stay fixed for the whole game.
- ALTERNATIVE: A global dynamic catalog that games read live; or raise `NUMBER_OF_CITIES`.
- COST: Roster plumbing through state, saves and UI; the roster is capped at 32 for 1.x.
- REVERSIBLE: The cap is; roster-per-game is a durable save property.

## W02-02 Stable namespaced keys, separate from names and runtime indices

- DECISION: `namespace:slug` keys (`classic`, `pthd`, `u-<hex>`, `qa-synthetic`); entity ids unique
  per definition; runtime indices derived (roster position, array position, tile index, Classic
  actor order, supplier number).
- BECAUSE: Renames, reordering and imports must not move businesses; saves already key properties
  by tile and suppliers by number (`SaveGameCity.cpp:32,681-682,716-727`).
- ALTERNATIVE: UUIDs only; or reuse numeric slots after 9.
- COST: An id registry and collision handling on import.
- REVERSIBLE: No for shipped keys.

## W02-03 Classic cities are never LocationDefinitions

- DECISION: `classic:*` entries are built by today's loader from the user's files
  (`CityFactory.cpp:70-87`) and are never converted, embedded or edited.
- BECAUSE: Classic must keep its fingerprint, including quirks (`CityFactory.cpp:132-139,535,697-703`),
  and original content must not end up in shareable files.
- ALTERNATIVE: Convert all ten to JSON and load everything through one path.
- COST: Two city sources to maintain.
- REVERSIBLE: Yes; a converter can still dump Classic data locally as reference.

## W02-04 Definitions realize topology; dynamic initialisation stays Classic

- DECISION: A definition stores every value `populate_properties` would draw; construction from it
  uses no RNG for topology, then runs the same dynamic initialisation calls in the same order as
  Classic (`CityFactory.cpp:82,85,582-594`).
- BECAUSE: Loading or regenerating content must not shift the simulation stream, while trends,
  advertising rates and the job market keep their Classic behaviour.
- ALTERNATIVE: Store a seed and re-run `populate_properties` at load.
- COST: Larger definitions; the Studio must author sizes and door offsets.
- REVERSIBLE: Yes, per contract version.

## W02-05 Embed admitted definitions in Expanded saves

- DECISION: Every non-Classic roster entry's admitted definition and hash are embedded; Classic
  entries are referenced; art is referenced and falls back.
- BECAUSE: A save must open after its bundle is deleted, renamed or left on another device;
  definitions are small (fixture: 51,566 bytes canonical, 3,385 compressed).
- ALTERNATIVE: An immutable required bundle per pinned revision.
- COST: Duplicated bytes across saves; budget 256 KiB per definition, 32 per roster.
- REVERSIBLE: Yes; a later "slim save" can reference bundles if needed.

## W02-06 Classic saves stay version 3; Expanded saves are version 4

- DECISION: The Classic writer keeps `version: 3` and adds no keys; Expanded saves are version 4
  with a `scenario` block. Loaders accept 1..4.
- BECAUSE: The fingerprint hashes the whole document (`SaveGameManager.cpp:1228-1249`), so any new
  Classic key would change it; old builds reject versions above their own
  (`SaveGameManager.cpp:388-389`), so an Expanded save can never be half-loaded.
- ALTERNATIVE: One bumped version for everything, with the fingerprint ignoring new keys.
- COST: Two writer constants and two code paths in `state_to_json`.
- REVERSIBLE: No for shipped saves.

## W02-07 Cosmetic state lives outside the fingerprint

- DECISION: Event mode, time zone, toggles, catalog pins and costume assignments go into an
  optional `presentation` block that `fingerprint()` drops, in both modes.
- BECAUSE: It gives a direct test that cosmetics never change the game, and a default Classic save
  stays byte-identical.
- ALTERNATIVE: Store cosmetics in settings only (lost with the save) or inside game state.
- COST: One erase in `fingerprint()`; every reader must treat the block as optional.
- REVERSIBLE: Yes.

## W02-08 The generator has its own RNG and integer arithmetic

- DECISION: PCG32 instance per stage, stage seed = SplitMix64(seed XOR FNV-1a-64(stage)); no floats
  in stored output; never `engine::random`.
- BECAUSE: The engine's two streams are global state (`Random.cpp:14-15`); per-stage streams let one
  stage be regenerated without shifting the others; integers give identical output on x64 and
  arm64.
- ALTERNATIVE: Reuse the engine LCG with a saved seed; or `std::mt19937` and floats.
- COST: A small RNG library and cross-platform determinism tests.
- REVERSIBLE: Per generator major version.

## W02-09 No floating point in content documents

- DECISION: Population weights are integers / 1000; seeds are decimal strings; hashes use RFC 8785
  JCS (equal to sorted compact JSON for these documents).
- BECAUSE: Float formatting differs between JSON libraries; hashes must match across Windows,
  Android and Python tools.
- ALTERNATIVE: Floats with a canonical formatter.
- COST: A conversion at construction.
- REVERSIBLE: Yes, by schema version.

## W02-10 Road rules come from `Car.cpp` and are Expanded-only errors

- DECISION: Lane continuity, one network, lane pairing and street access are validator errors for
  definitions, derived from the car tables (`Car.cpp:87-104,171-223,253-307`).
- BECAUSE: Cars drive straight over anything after a dead end; nothing in the economy uses roads
  (`Property.cpp:138-213`), so these are consistency rules for believable maps, and the Classic
  maps are not judged by them.
- ALTERNATIVE: Simulate deliveries on roads.
- COST: Classic maps may not satisfy every rule (to measure locally).
- REVERSIBLE: Yes.

## W02-11 Admission states and a strict counting rule

- DECISION: draft -> validated -> integrated-candidate -> gameplay-accepted, each with a receipt
  matched by definition hash; publication needs all three; only gameplay-accepted new locations
  count toward 25 new / 35 total.
- BECAUSE: A picture, a schema-valid file or a fast catalog is not a playable location.
- ALTERNATIVE: draft / published only.
- COST: Automated probes per location and per qualified platform.
- REVERSIBLE: States can be added; receipts are durable.

## W02-12 One layered project document with a revision tree

- DECISION: Quick Create, Guided Create and the Full Editor edit one document: a working
  definition, layers, locks, manual-field provenance, generation inputs, a revision tree with redo,
  branches and receipts. Locks are generator inputs.
- BECAUSE: Mode switches must not lose work, and selective regeneration must keep manual edits.
- ALTERNATIVE: One document per mode with converters.
- COST: Revision storage and compaction (2,000 revisions per document).
- REVERSIBLE: Yes, by schema version.

## W02-13 Eight regions, a separate bonus route, markers from geodata

- DECISION: Eight regions with crop bounds from `region-overviews-v1/manifest.json`; the North Pole is
  a bonus route outside every region; marker positions are projected from geo, never stored or taken
  from art.
- BECAUSE: Geography must stay factual and independent of decorative art; a fictional village must
  not sit inside a real country.
- ALTERNATIVE: Hand-placed markers on painted maps.
- COST: A pinned gazetteer for every real location.
- REVERSIBLE: Region crops are tunable data.

## W02-14 Paging: 12 or 6 per page, no wrap, selection by key

- DECISION: Desktop and tablet 12, phone 6 (short side below 600 dp); `ceil(N / size)`; clamp after
  changes; previous/next stop at the ends; selection follows the key.
- BECAUSE: The user's sizes; wrapping across a filtered list surprises; keys survive deletes and
  imports.
- ALTERNATIVE: Wrap-around; fixed page counts per region.
- COST: Clamping logic and tests (fixtures provided).
- REVERSIBLE: Yes.

## W02-15 Facets are many-to-many discovery tags

- DECISION: country, state, community, biome, setting and season tags on definitions and catalog
  entries; never identity, never simulation input.
- BECAUSE: Discovery must not stereotype or replace research, and border places need several tags.
- ALTERNATIVE: One country per location as part of its id.
- COST: Curated vocabularies.
- REVERSIBLE: Yes.

## W02-16 Objectives are existing milestones; goals can continue as sandbox

- DECISION: Scenario objectives are MilestoneIds with a verified emitter (registry v1); `avoid`
  objectives use loan and financial-difficulty milestones; a new observer shows the goal summary and
  continues; presets set `win: none`.
- BECAUSE: No new mechanic is needed, and both current endings remove the player
  (`GameState.cpp:519-531`, `WinSequenceView.cpp:97`).
- ALTERNATIVE: A scripting language for objectives; reuse mission mode.
- COST: One observer and a goal-reached flag in Expanded saves.
- REVERSIBLE: The registry can grow.

## W02-17 Santa's Pizza Village presets use a single-location roster

- DECISION: Cozy Village, Workshop Rush and Pizza Empire share the roster
  `[pthd:santas-pizza-village]`, travel disabled, always available, no date lock.
- BECAUSE: City-scoped milestones are not tagged with a city, so one location keeps objectives
  unambiguous; year-round play was required.
- ALTERNATIVE: Add the village to a world roster.
- COST: The village cannot be combined with other cities in 1.0 presets.
- REVERSIBLE: Yes, with location-scoped milestones later.

## W02-18 Calendar events are pure, offline and presentation-only

- DECISION: Modes off, in-game calendar, real date (opt-in, IANA time zone, local midnight) and a
  manual preview; resolution is a pure function of catalog, mode, date, location and toggles;
  overlays join presentation layers only; animated overlays need a static replacement.
- BECAUSE: Events must never change economy, collisions, room access, ownership or RNG, must work
  offline and must never lock content.
- ALTERNATIVE: Events as gameplay modifiers; online calendars.
- COST: A sourced catalog and reduced-motion art for every animated overlay.
- REVERSIBLE: Yes.

## W02-19 Cast and costumes bind to roles the engine draws today

- DECISION: Stable `holiday:` ids; roles staff (waiter, chef, manager), customer, host (presenter
  portrait) and decorative; frame contracts copied from the draw code; a costume resolver checks
  frames before loading and falls back per frame; `economics: none`.
- BECAUSE: A missing BOB is fatal (`BobLoader.cpp:25-27`) and a bad frame index asserts
  (`Bob.cpp:38`); costumes must never hide a character, crash a load or change skills.
- ALTERNATIVE: Cast members as new hireable characters with stats.
- COST: Frame production per contract; a resolver in the draw path.
- REVERSIBLE: Yes.

## W02-20 Geodata is authoritative; art goes through admission

- DECISION: Natural Earth (pinned release, hash, licence) for coastlines and crops, a pinned gazetteer
  for coordinates; every art asset carries an admission record (candidate, curated, style-reviewed,
  runtime-validated, admitted); unknown rights and generic references block admission.
- BECAUSE: Generated images can invent coastlines and places; presence of a file is not acceptance.
- ALTERNATIVE: Treat generated overviews as maps.
- COST: Curation work and records for every asset.
- REVERSIBLE: Yes.

## W02-21 UI-only QA catalogs are marked twice

- DECISION: Synthetic entries use the `qa-synthetic:` namespace and their catalogs set `ui_only`;
  the runtime refuses to build them.
- BECAUSE: 1,000 fake entries must never appear as playable content.
- ALTERNATIVE: A test-build flag only.
- COST: None.
- REVERSIBLE: Yes.

## W02-22 Keep the 160 x 120 grid in contract 1.x

- DECISION: The schema fixes 160 x 120 tiles and 20 x 15 cells (Codex ADR-02).
- BECAUSE: Map size appears in many places, including literals (`Car.cpp:23`,
  `RestaurantEconomy.cpp:202,204`); location count is changed first.
- ALTERNATIVE: Variable sizes now.
- COST: All new areas share one size.
- REVERSIBLE: Yes, by a later contract version.
