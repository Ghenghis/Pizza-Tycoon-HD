# Location, catalog and content fixtures (contract W02)

Fixtures for `docs/studio/location-contract.md` and the schemas beside it. JSON only, no
original game data. `expectations.json` holds the same table in machine-readable form. Rule
ids are defined in the contract (sections 5, 10, 11, 12, 14, 15). `SCHEMA-1` means JSON Schema
validation fails, so the semantic rules are not run.

All 160x120 maps here are hand-built test maps (tile codes chosen to exercise the rules), not
generated content. The `qa-synthetic:` catalogs are **UI-only**: the runtime must refuse to
start or travel into any of their entries.

## Location definitions (`location.schema.json`)

- `valid/harbor-bay.location.json`: must pass. A 4-district map with one two-lane avenue
  and one two-lane street crossing at a 2x2 junction, 23 business premises and 1 depot, all
  facing the street, every actor role, one supplier per category.
- `invalid/disconnected-roads.location.json`: must fail `ROAD-3`: a second, self-consistent
  L-shaped road in the south-west touches no other road (two road networks).
- `invalid/unreachable-door.location.json`: must fail `PROP-3`: premise `p-backlot` at (30,30)
  faces a residential tile, not a road or path of the network.
- `invalid/impossible-budget.location.json`: must fail `ECON-1`: rent and price levels of 4000
  leave no premise affordable with the declared reference cash ($50,000).
- `invalid/duplicate-ids.location.json`: must fail `ID-1`: two properties share id `p-01`.
- `invalid/out-of-bounds.location.json`: must fail `SCHEMA-1` at `/properties/0/tile/x`
  (160 is outside 0..159).
- `invalid/district-cell-out-of-range.location.json`: must fail `REF-2`: cell (0,0) names
  district 7 of 4 (the unchecked index at `CityFactory.cpp:699`).
- `invalid/missing-mayor.location.json`: must fail `ACT-1`: no mayor (`CityFactory.cpp:572`
  would dereference an empty optional).
- `invalid/owned-shop-front.location.json`: must fail `TILE-1`: tile 0x3A, a player-coloured
  shop front, at (10,57) (the `map1.dat` defect fixed at `CityFactory.cpp:132-139`).
- `invalid/bonus-with-geo.location.json`: must fail `WORLD-2`: a bonus destination carries a
  geographic coordinate.

## Studio project (`project.schema.json`)

- `valid/harbor-bay.project.json`: must pass. Wraps the valid definition with a roads layer
  lock, an object lock, two revisions and per-field provenance.

## Catalog (`catalog.schema.json`)

- `catalog/world-catalog-v1.json`: must pass, warning `CAT-3` (Natural Earth source not yet
  pinned to a release). The ten Classic entries, eight regions with their crop bounds from
  `world-region-art/region-overviews-v1/manifest.json`, the North Pole bonus route and the
  25-item content plan. No new location is a catalog entry yet.
- `catalog-qa/qa-35.catalog.json`: UI-only, must pass. 35 entries as `[6,3,10,3,3,3,3,3]` plus
  1 bonus (the shape of 10 Classic + 24 + 1).
- `catalog-qa/qa-64.catalog.json`, `qa-96.catalog.json`, `qa-1000.catalog.json`: UI-only, must
  pass. 8, 12 and 125 entries per region.
- `catalog-qa/qa-uneven-empty.catalog.json`: UI-only, must pass. `[0,1,13,0,25,6,7,2]`: two empty
  regions, partial last pages (13 on desktop = 12 + 1).
- `catalog-qa/expected-pages.json`: oracle. Page counts per region and for All locations, last
  page sizes, first and last key in sort order, empty states.
- `catalog-qa/page-math-cases.json`: oracle. `ceil(N / pageSize)`, page clamping after delete,
  filter and import, selection kept by key.

## Scenarios (`scenario.schema.json`)

- `scenarios/north-pole-cozy-village.scenario.json`, `north-pole-workshop-rush.scenario.json`,
  `north-pole-pizza-empire.scenario.json`: must pass. Content drafts: not playable until
  `pthd:santas-pizza-village` is gameplay-accepted.
- `scenarios/invalid/unsupported-milestone.scenario.json`: must fail `SCHEMA-1` at
  `/objectives/goal/all_of/0/milestone` (`ControlsWorld` is not in objective registry v1).
- `scenarios/invalid/win-ends-sandbox.scenario.json`: must fail `SCN-3`: a market-share win
  ends the game, which contradicts `after_goal: continue-sandbox`.

## Events (`event-catalog.schema.json`)

- `events/core-events.event-catalog.json`: must pass.
- `events/resolution-cases.json`: oracle. Off, in-game calendar, year boundary, secular
  alternative, North Pole in July and December, real date in two time zones, manual preview,
  reduced motion.
- `events/invalid/flashing-without-reduced-motion.event-catalog.json`: must fail `EVT-4`.

## Cast (`cast-catalog.schema.json`)

- `cast/holiday-cast.cast-catalog.json`: must pass. 16 characters and 4 costumes (20 entries).
  Concept states are as read from batch `20261114T183935Z` while it was still running; no frame
  exists yet, so every frame set is `not-started`.

## Asset admission records (`asset-admission.schema.json`)

- `assets/toronto-candidate.asset.json`, `north-pole-candidate.asset.json`,
  `world-overview-geodata.asset.json`: must pass (state `candidate`).
- `assets/invalid/admitted-unknown-rights.asset.json`: must fail `SCHEMA-1` at
  `/rights/status` (unknown rights block admission).
- `assets/invalid/admitted-generic-reference.asset.json`: must fail `SCHEMA-1` at
  `/authenticity` (a generic modern reference cannot be admitted as location art).
