# Location, content and save contract (W02)

Contract version **1.0.0-review**. It becomes 1.0.0 (frozen) when the integrator accepts it;
after that, changes are additive minor versions or a new major version with a migration.

This is a design. **Nothing in it is implemented.** Every statement about the current engine
cites `file:line` in this repository as of commit `1fd2a9e`. The Codex review on the owner's
PC is pinned to `94d616e`; some of its line numbers differ by a line or two from the ones here
(for example its `WeaponMarket.h:47` is `:48` here). `SAVE_FORMAT_VERSION` is 3
(`src/core/save/SaveGameManager.h:26`).

Files that belong to this contract:

| File | What it fixes |
|---|---|
| `location.schema.json` | LocationDefinition: one playable Expanded location |
| `project.schema.json` | The canonical Studio document (Quick Create, Guided Create, Full Editor) |
| `scenario.schema.json` | Scenario and preset: roster, start, rules, objectives |
| `catalog.schema.json` | World catalog index: regions, bonus routes, entries, paging, menu data, content plan |
| `event-catalog.schema.json` | Optional calendar events |
| `cast-catalog.schema.json` | Characters, costumes, role bindings, frame contracts |
| `asset-admission.schema.json` | Admission record for art and geodata assets |
| `adr.md` | The decisions, with alternatives and costs |
| `tests/fixtures/locations/` | Valid and invalid fixtures, UI-only QA catalogs, oracles |

Section map: 1 inventory, 2 identity, 3 LocationDefinition, 4 construction, 5 validation,
6 saves, 7 generation, 8 admission and publication, 9 Studio project, 10 catalog and
navigation, 11 scenarios and objectives, 12 calendar events, 13 main menu data, 14 cast and
costumes, 15 geography, art and asset admission, 16 refactor order, 17 unrelated projects,
18 defects found while verifying, 19 open questions.

---

## 1. Inventory: fixed city count and city-id indexing

"Classic" below means the exact 1994 game; "Expanded" means a scenario with its own roster
(section 2.3). The **change** column says what the refactor in section 16 does; Classic keeps
the same values everywhere (roster size 10, roster index = today's city id).

### 1.1 The count itself and arrays sized by it

| # | Location | What it indexes or assumes | Change |
|---|---|---|---|
| 1 | `src/core/util/Constants.h:26` | `NUMBER_OF_CITIES = 10` | Stays as the Classic roster size only; no new code may use it |
| 2 | `src/core/state/GameState.h:104-105` | `is_valid_city_id`: `0 <= id < 10` | `id < roster.size()` |
| 3 | `GameState.h:235` | `CityInitFlags = std::array<bool, 10>` | vector sized by roster; JSON array of roster length |
| 4 | `GameState.h:545` | `cities_`: `std::array<std::unique_ptr<City>, 10>` | vector sized by roster |
| 5 | `src/core/state/GameState.cpp:95` | `operator<<` loops 0..9 | loop roster |
| 6 | `GameState.cpp:216-219`, `:246-249`, `:253-256` | `city()`, `is_city_loaded()`, `active_city()` index `cities_[id.value]` | unchanged code, bounds from roster |
| 7 | `GameState.cpp:279-287` | `city_or_create` builds a city from `CityFactory::create_city(city_id, ...)` by number | dispatch through the roster entry's source (section 4) |
| 8 | `GameState.cpp:289-299` | `default_city_id` loops 0..9, falls back to `CityId{0}` (`:298`) | roster loop; fallback = scenario start location |
| 9 | `GameState.cpp:301-314` | `active_city`: `city_initialised_[id.value]` | roster-sized flags |
| 10 | `src/core/state/TurnOrchestrator.h:65-67` | `CityDayFlags`, `RestaurantDayFlags` = arrays of 10 (x 8 players) | roster-sized |
| 11 | `TurnOrchestrator.h:84,87` | `city_ai_action_taken_[city.value]` | roster-sized |
| 12 | `src/core/state/TurnOrchestrator.cpp:1280-1281`, `:1367-1369`, `:1391`, `:1466-1473` | `cities_[city_id.value]`, flag fills, `restaurant_day_ran_[city][player]`, `city_start_of_day_ran_[idx]` | roster-sized |
| 13 | `src/core/character/Player.h:163-169`, `:279` | `turnover_by_city_`: `std::array<Money, 10>` read with `.at(city.value)` | roster-sized |
| 14 | `src/core/finance/PlayerFinancials.h:82-87`, `:111-112`; `PlayerFinancials.cpp:97`, `:123-124` | `weekly_city_profit_`, `days_present_`: arrays of 10 | roster-sized |
| 15 | `src/core/economy/WeaponMarket.h:25`, `:47-48`; `WeaponMarket.cpp:50` | `slot_count = 10 * 4` price slots; assert `city < 10` | roster x 4 slots |
| 16 | `WeaponMarket.cpp:93-97`, `:100-115`; called at `GameState.cpp:1105` and `TurnOrchestrator.cpp:1376` | `init()` and `daily_update()` draw simulation RNG **per slot** | Classic must keep exactly 40 slots so the draw count is unchanged; an Expanded roster of N draws for 4N slots, fixed at game creation |
| 17 | `src/core/conversations/MafiaConversation.cpp:351` | tip city `rand_max_n(NUMBER_OF_CITIES)`: simulation RNG bound | `rand_max_n(roster.size())` (10 in Classic, same draw) |
| 18 | `src/core/state/MilestoneTracker.cpp:28-37` | `owned_city_count` uses `std::array<bool, 10>` | roster-sized |
| 19 | `MilestoneTracker.cpp:265-277` | `opened_cities` array of 10 | roster-sized |
| 20 | `src/core/save/SaveGameManager.cpp:633-641`, `:856-866` | weapon market saved and loaded as `10 * 4` positional slots | roster x 4 |
| 21 | `src/core/save/SaveGameCharacter.cpp:347-351`, `:572-580`, `:1054-1057` | `turnover_by_city`, `weekly_city_profit`, `days_present` read and written positionally for 10 cities (`get<std::array<int64_t, 10>>`) | roster length |
| 22 | `src/views/screens/PlayerProfile.cpp:531` | profile page draws one row per city 0..9 | roster rows (Expanded needs a scrolling list) |
| 23 | `src/core/debug/command/CommonArgs.h:49`; `src/core/debug/command/ValueValidator.cpp:195`; `src/core/debug/commands/ActorCommands.cpp:846` | console `city_id` range 0..9, "Valid city IDs: 0-9", arms market dump loop | roster range; console also accepts a location key |

### 1.2 Arithmetic and literals on the city number

| # | Location | Assumption | Change |
|---|---|---|---|
| 24 | `GameState.cpp:910-913` | arrival milestone = `ArrivalInParis (100) + city_id.value` | only for roster entries with a classic number, using that number (section 11) |
| 25 | `MilestoneTracker.cpp:183-184` | domination milestone = `DominatesMarketInParis (110) + city.id().value` | same rule |
| 26 | `MilestoneTracker.cpp:219-220` | `WinCondition::Paris` checks `CityId{0}` | roster lookup of `classic:paris` |
| 27 | `MilestoneTracker.cpp:240-242` | `AllCities` requires 10 initialised cities | requires every roster entry |
| 28 | `MilestoneTracker.cpp:312-315` | `OutletsInEveryWesternCity` needs restaurants in 10 cities | "every roster location" |
| 29 | `MilestoneTracker.cpp:318-331` | warehouse milestones at 10 and 5 cities | thresholds unchanged; unreachable when the roster is smaller (objective validator warns) |
| 30 | `src/core/events/MilestoneId.h:126-146`; `src/core/events/CareerLog.cpp:130-149` | milestone ids 100-119 and their scores, one per classic city | unchanged; Classic only |
| 31 | `src/core/city/CityNames.h:18-21`, `:23-28` | 10 names indexed by id; `"Unknown"` outside 0..9 | Classic adapter table; Expanded names from the definition |
| 32 | `src/views/screens/EuroMap.cpp:41-52` | `city_coords[]` indexed by id | Classic only; Expanded uses section 10 |
| 33 | `EuroMap.cpp:78-100` | one click region per city 0..9; interaction id `"euromap.{name}"` (`:82`) | Classic only (ids unchanged) |
| 34 | `EuroMap.cpp:262-269` | travel cost = Manhattan distance of `city_coords` x (difficulty + 1) | Classic only; Expanded `geo_manhattan` (scenario rule) |
| 35 | `EuroMap.cpp:304-306` | New Game city choice writes `initializer().city_id` | Expanded: start location from the scenario |
| 36 | `GameState.cpp:1358` | bootstrap quick game uses `CityId{6}` (Madrid) | debug only; leave |
| 37 | `src/core/tutorial/TutorialGame.cpp:37,40,56-57`; `src/core/tutorial/TutorialSteps.h:17` | tutorial: seed 292, city 6, premise tile (119, 91) | Classic only |
| 38 | `src/views/screens/restaurant/RestaurantView.cpp:1494` | debug fallback `active_city(CityId{0})` | debug only; leave |
| 39 | `src/core/init/NewGameConfig.h:40` | `Config::city_id{0}` | Expanded adds a roster and start key |

### 1.3 Content found by number

| # | Location | Assumption | Change |
|---|---|---|---|
| 40 | `src/core/factories/CityFactory.cpp:70-87` | `create_city(id)` loads `dat/city{id}.pc`, `dat/bez{id}.dat`, `dat/map{id}.dat` (`:96`, `:144`, `:744`) | `ClassicCitySource` keeps this exactly; `DefinitionCitySource` builds from a definition (section 4) |
| 41 | `CityFactory.cpp:132-139` | `fix_legacy_bugs` patch for city 1, tile 11589 | Classic only |
| 42 | `SaveGameCity.cpp:512-514` and `SaveGameManager.cpp:889-907` | a save stores only the city number and rebuilds the city from the original files on load | Expanded saves carry the definition (section 6) |

### 1.4 Map geometry (kept at 160x120 by ADR-02, listed for later)

`Constants.h:15-18`; `src/core/city/City.h:106-113` (index helpers), `:132`, `:391`
(`std::array<uint8_t, 300>`); `CityFactory.h:67`; `src/core/city/City.cpp:491-503`;
`src/core/city/ControlMap.h:18-19,43,51-67`; `ControlMap.cpp:66-68,116`; `SaveGameCity.cpp:289-307`;
`src/core/city/Car.cpp:16-17`, `:23` (**literal 160**), `:112-114`; `CarManager.cpp:76,231-239`;
`src/core/city/Property.cpp:157-160`; `src/core/restaurant/RestaurantEconomy.cpp:202,204`
(**literals 159/119**); `CityView.cpp:133-134,732-754,793-799,847-848,1509-1510,1761-1762`;
`ZoomConfig.h:45-46,58-59,72-74,92-93`; `StatsRestaurants.cpp:136-137`;
`CityCommands.cpp:438,472`. Contract 1.x fixes the grid (`location.schema.json` `grid` consts);
none of these change for the first Expanded release.

### 1.5 Simulation RNG drawn while a city is built

City creation is lazy (`GameState.cpp:279-287`): a city is built the first time it is entered,
and building it draws the **simulation** stream (`src/engine/Random.cpp:14,59-73`):
`populate_properties` (`CityFactory.cpp:702-726`: sizes, door offsets, realtor, status),
`init_advertising_options` (`CityFactory.cpp:582`, draws in `City.cpp:98-114`), ten economy
trend ticks (`CityFactory.cpp:589-592`), `seed_ingredient_trends` (`:594`, `:598-615`) and the job
market (`:85`). On load, cities are rebuilt the same way (`SaveGameManager.cpp:897`) and the RNG
state is restored afterwards (`:1108-1109`), which is why load stays deterministic. Two
consequences for this contract: an Expanded definition **realizes** everything
`populate_properties` would draw (section 4), and no browse, preview or Studio code may call
`GameState::city_or_create`, `GameState::active_city` or `Property::calculate_demographics`
(the latter draws simulation RNG for edge tiles, `Property.cpp:189-190`).

---

## 2. Identity

### 2.1 Content ids and runtime indices

| Thing | Content id (stable, in documents and saves) | Runtime index (per game) |
|---|---|---|
| Location | `location_key` = `<namespace>:<slug>`, pattern `^[a-z][a-z0-9-]{1,31}:[a-z0-9][a-z0-9-]{1,63}$` | `CityId.value` = position in the scenario roster |
| District | entity id, unique in the definition | `DistrictIndex` = position in `districts[]` (cells store it) |
| Property | entity id | tile index `y * 160 + x`, as saves already use (`SaveGameCity.cpp:32`, `:681-682`) |
| Actor | entity id | `ActorId` assigned in Classic order: realtors, banks, insurers, mayor, police, mafia, arms dealers (the order `CityFactory.cpp:514-569` produces) |
| Food supplier | entity id | `number` (u8), which saves match on (`SaveGameCity.cpp:716-727`) |
| Scenario | `scenario_id` (same pattern as locations) | none |
| Character, costume | `<namespace>:<slug>` (`holiday:santa`) | none (presentation) |
| Event | `<namespace>:<slug>` (`pthd-events:christmas-decor`) | none |
| Asset | `asset_id` path-like (`location-art/toronto-ontario-canada`) + `revision` | none |

Namespaces: `classic` (reserved, the original ten), `pthd` (first-party Expanded content shipped
with the game), `u-<8 hex>` (a local Studio installation; the hex is generated once per
installation), imported bundles keep their namespace, `qa-synthetic` (UI tests only; the
runtime refuses to build these). Entity ids match `^[a-z][a-z0-9-]{0,47}$` and are unique across
the whole definition (one namespace for districts, properties, actors and suppliers, so change
selectors never need a kind to disambiguate).

Rules: the display name never feeds an id. Renaming changes `display.name` only. A published
revision is immutable; edits create revision + 1. Runtime indices may differ between revisions,
which is safe because a game pins exact revisions (section 6). Import of a key that exists
locally with a different lineage is refused, or imported under a new key (user choice); it
never overwrites.

### 2.2 Classic adapter

The ten Classic cities are not LocationDefinitions. They are built from the user's own game
files by today's code (`ClassicCitySource`) and identified as:

| Classic number (today's `CityId`) | Key | Name (`CityNames.h:19-20`) | Region | EuroMap coord (`EuroMap.cpp:42-51`) | Arrival / domination milestone |
|---|---|---|---|---|---|
| 0 | `classic:paris` | Paris | europe | (237, 95) | 100 / 110 |
| 1 | `classic:berlin` | Berlin | europe | (289, 88) | 101 / 111 |
| 2 | `classic:baltimore` | Baltimore | north-america | (67, 80) | 102 / 112 |
| 3 | `classic:vienna` | Vienna | europe | (282, 111) | 103 / 113 |
| 4 | `classic:chicago` | Chicago | north-america | (7, 53) | 104 / 114 |
| 5 | `classic:new-york` | New York | north-america | (79, 61) | 105 / 115 |
| 6 | `classic:madrid` | Madrid | europe | (175, 162) | 106 / 116 |
| 7 | `classic:rome` | Rome | europe | (276, 149) | 107 / 117 |
| 8 | `classic:stockholm` | Stockholm | europe | (301, 42) | 108 / 118 |
| 9 | `classic:london` | London | europe | (223, 67) | 109 / 119 |

Quirks the adapter preserves because they are part of the Classic fingerprint: the stray shop
front patch (`CityFactory.cpp:132-139`), the district lookup through a truncated cell index
(section 18), positional actor roles (`CityFactory.cpp:521-528`), and every RNG draw listed in
1.5. Region markers and geographic coordinates for the ten come from the gazetteer (section 15),
not from the EuroMap picture.

### 2.3 Scenario roster

A game has one **roster**: an ordered list of location keys, each pinned to an exact revision
and definition hash (or to `classic`). Roster position = runtime `CityId`. The roster is chosen
at game creation, copied into the save, and **never changes** during that game. Installing,
updating, renaming or deleting catalog content cannot add a city to a running game, change
array sizes, change victory scope or change RNG draw counts (item 16 in 1.1). Classic games use
the implicit Classic roster `[classic:paris .. classic:london]`. Limits: 1..48 entries in
contract 1.x. That is a **provisional active-simulation budget** per game on desktop (the 35
locations of the full target fit with room), not a catalog limit: the catalog has no ceiling,
every shipped area stays discoverable and user additions are unlimited. A phone budget is set
only from measurements on the real device (S21, tablet); a game that would not fit is refused
before it starts, never by dropping locations or saves.

---

## 3. LocationDefinition

Schema: `location.schema.json`. Example: `tests/fixtures/locations/valid/harbor-bay.location.json`.
The document has **no floating-point numbers** (population weights are integers / 1000), so its
canonical form for hashing is RFC 8785 JCS, which for integer-and-string documents equals
sorted-key compact UTF-8 JSON.

### 3.1 Grid and tiles

Fixed 160 x 120 tiles and 20 x 15 district cells of 8 x 8 tiles (`Constants.h:15-18`). `tiles`
is 120 rows of 320 hex digits; byte meaning is the `dat/mapN.dat` tile code, classified with the
engine's own predicates:

| Code | Meaning | Engine predicate |
|---|---|---|
| low nibble `0xA`, high nibble 8..F | unowned business premise (one `Property` per tile) | `City::is_business_premise_by_type`, `City.cpp:124-133` |
| low nibble `0xA`, high nibble 0..7 | **owned** shop front of player 0..7, drawn by `City::type()` (`City.cpp:171-186`) | forbidden in definitions (TILE-1) |
| `0x04` | depot premise | `City.cpp:117-122`, `CityFactory.cpp:691-696` |
| `0x05` | depot building, no property | `City.cpp:240-246` |
| low nibble `0x8`, high nibble 8..F | named building church .. hotel | `City.cpp:263-269`, `:224-238` |
| low nibble 6 or 7 except `0xE6`, `0xF6` | road | `City.cpp:144-148` |
| low nibble 0 / 3 / 4 / 5 / 9 / B | nature / water / building / path / villa / residential | `Property.cpp:237-258` (population weights) |

`CityFactory::populate_types` (`CityFactory.cpp:617-677`) classifies by the **high** nibble and
disagrees with the road rule above; its result (`City::tile_category`) has no caller outside
`City.cpp`, so it is not a gameplay rule and the contract ignores it.

### 3.2 Roads (rules from `Car.cpp`)

Each road code is one **lane** with a driving direction. On entering a tile a car takes the
direction(s) of that tile (`Car::decide_desired_direction`, `Car.cpp:171-223`):

| Out set | Codes |
|---|---|
| W | `0x06` ROAD_W, `0xD6`, `0x17`, `0xA7`, `0x47`, `0x86` CORNER_W, `0xB7` (see TILE-3) |
| E | `0x16` ROAD_E, `0xC6`, `0x97`, `0xD7`, `0xA6` CORNER_E |
| N | `0x36` ROAD_N, `0x37`, `0x07`, `0xC7`, `0x96` CORNER_N, `0x57` |
| S | `0x26` ROAD_S, `0xF7`, `0xE7`, `0xB6` CORNER_S, `0x67` (see TILE-3) |
| W or S | `0x46` CORNER_SW, `0x87` (see TILE-3) |
| S or E | `0x56` CORNER_SE |
| W or N | `0x66` CORNER_NW, `0x27` |
| E or N | `0x76` CORNER_NE |
| none (car keeps going straight, across anything) | every other code, including `0x77` |

Two-way corners choose by a presentation-RNG coin unless the car just turned left on that tile
(`Car.cpp:240-251`). A car that leaves the visible area moves to the **return lane** and reverses:
east -> one row north heading west, west -> one row south heading east, north -> one column west
heading south, south -> one column east heading north (`Car.cpp:87-104`). Cars are presentation
only: they use `engine::random::presentation` (`Car.cpp:105,237,248`; `CarManager.cpp:243-246`) and
nothing in the economy reads them. Deliveries do not use roads; the catchment is a Manhattan
radius (`Property.cpp:138-213`). Road rules are therefore consistency rules for a believable,
working city view, not simulation rules, and they are errors only for Expanded content:

- **ROAD-2 continuity:** for every road tile and every direction d in its out set, the next tile
  in d is off the map, or a road tile with a non-empty out set that does not contain the
  reverse of d (no dead end, no U-turn). A car crosses `0x77` in a straight line, so the check
  steps over `0x77` tiles in d before it looks (calibration: the ten original maps use `0x77`
  529 times as the middle of junctions, and with this rule none of them has a dead end).
- **ROAD-3 one network:** all road tiles form one 4-connected component.
- **ROAD-4 lane pairing:** every straight lane tile (non-corner code with one out direction) has
  a road tile on its return side, because the wrap above can happen at any viewport edge.
- Map-edge exits are allowed. Three codes drive differently from how they spawn
  (`0xB7`, `0x67`, `0x87`: compare `Car.cpp:179,203,207` with `:263,266,281`); generators must not
  emit them and the validator warns (TILE-3).

The fixture's junction shows a legal crossing of a two-lane avenue (W lane row 58, E lane row 59)
and a two-lane street (S lane column 78, N lane column 79): `0x46` `0x66` over `0x56` `0x76`.

### 3.3 Districts

1..16 districts (cells are read through the low nibble, `City.cpp:484-487`). Fields map one-to-one
to `DistrictDataRaw` (`CityFactory.cpp:36-64`) as decoded by `load_districts` (`:320-385`):
pizza-count minimums are stored decoded (`/10`, `>>4`). Population weights are integers in
thousandths (runtime float = value / 1000). `district_cells` values must be below the district
count (REF-2): the Classic loader indexes the district vector with no check
(`CityFactory.cpp:699`).

### 3.4 Properties, premises and entrances

One property record per premise tile and none elsewhere (PROP-1). A property is a single map
tile with an interior size, as in Classic (`CityFactory.cpp:689-739`):

| Field | Range and rule | Basis |
|---|---|---|
| `interior.w/h` | 10..20 | `min/max_premise_side`, `CityFactory.cpp:680-681,708-709` |
| `kitchen.w` | 12..20 | `min_kitchen_width`, `:682,713-714` |
| `kitchen.h` | 7..20; warning outside `[w-3, w]` | `kitchen_h = w - rand(4)`, `:715` |
| `entrance_offset` | 1..`w-2` (PROP-2) | the doorway is 3 tiles wide in the bottom wall (`WalkNavigation.cpp:241-261`); Classic draws it from `h` (`CityFactory.cpp:717`), see section 18 |
| `kitchen_entry_offset` | 0..`kitchen.h-7` | `CityFactory.cpp:718` |
| `street_side` | the neighbour on that side is a road or path tile of the main network (PROP-3) | new rule; Classic has no street link |
| `realtor` | id of a realtor actor (REF-1) | `CityFactory.cpp:720` draws an actor id with no type check |
| `initial_status` | not_available / for_sale / for_rent / for_sale_rent | `PremiseStatus`, `CityTypes.h:13-24` |
| `purchase_price_cents` | warning unless `price_level x w x h x 100` (x 5 for depots) | `CityFactory.cpp:730-733`, `depot_price_multiplier` (`Property.h:29`) |

Rent is not stored: `rent_level x w x h` dollars per month (`Property.cpp:276-282`). The furniture
layout of a quick-start or AI restaurant replaces the premise size (`RealEstate.cpp:448`). At
least 16 business premises (PROP-5): every AI player may open a starting restaurant on a random
premise (`GameState.cpp:268-277`, `RealEstate.cpp:359-379,417-438`), and quick start needs one too.
Interior walkability of furnished rooms is a Studio layout rule, not a definition rule: today's
placement checks only walls and overlaps (`FurniturePlacementController.cpp:291-308,375-395`).

### 3.5 Actors

Roles are explicit lists. Required counts (ACT-1): realtors 1..10, banks 1..10, insurers 1..10,
exactly one mayor, police 1..9, mafia 0..2, arms dealers 0..8, mafia + arms dealers <= 10. Basis:
`*police_id` and `*mayor_id` are dereferenced unconditionally (`CityFactory.cpp:571-572`), other
code looks the mayor up by type (`GameState.cpp:807`), a realtor id is drawn for every premise
(`:720`), and at most 10 per type and 2 mafia are positional (`CityFactory.h:35`,
`CityFactory.cpp:521-528`). Heads are 1..25: `ActorView` indexes 25-entry eye tables with
`head - 1` (`ActorView.inl:28-36,290-299`); supplier heads go through the same view
(`SupplierConversation.cpp:62`). Phone numbers are stored decoded; non-zero numbers are unique
(ID-2). Per-type settings are the record fields read at `CityFactory.cpp:751-849`.

### 3.6 Food suppliers and prices

Categories vegetables, meat, fruit, cheese (the loader skips the cut categories 4..7,
`CityFactory.cpp:284-306`). 22 price levels per supplier, 0 = not sold (`:274-279`). `number`
is unique (saves match on it). At least one supplier per category (SUP-1); every valid
ingredient sold by someone (SUP-2, warning); `ingredient_base_prices` has 96 entries in cents and
is non-zero for every valid ingredient id (SUP-3; ids per `docs/blueprint/content-data.md` 1.2).

### 3.7 Demographics and economy templates

Districts carry realized values (income factor, demographic base/factor, weights, price and rent
levels, availability). Templates are generation inputs only: `economy.template` records which
template revision produced the values, and the runtime never looks it up. `economy.startup`
declares the reference cash and the minimum number of affordable premises for ECON-1; the
scenario check at game creation repeats the test with the chosen characters' real starting money
(`CharacterFactory.cpp:95-96`).

### 3.8 Asset bindings and fallbacks

`assets.tile_atlas` and `medium_atlas` bind the city tiles to `classic:gfx/landsym` / `gfx/med1`
(the user's original atlases, `AssetMetadata.cpp:82`, with HD packs applied as today) or to a
`bundle:` indexed atlas. Fallback chain: bundle art -> classic art from the user's copy ->
engine placeholder (flat colours). `assets.kit` names the region kit (section 10.6).
`attachments` are optional art (thumbnails, HD overlays) with hashes and admission state; they
never change tiles or any simulation input, and a location is playable with all of them missing.
Generated art alone is never a playable map.

### 3.9 World placement

`world.placement` is a real region (one of 8) or the bonus route `north-pole`. `world.geo` is a
coordinate in micro-degrees from a pinned gazetteer, or a user-placed point for fictional
locations, and must be null for a bonus destination (WORLD-2). Overview marker positions are
**derived** from geo (section 10.3); they are not stored, so art can never move a location.
`facets` hold the discovery tags (section 10.5).

---

## 4. Construction from a definition

`CityFactory::create_city` (`CityFactory.cpp:70-87`) becomes a dispatch:

- `ClassicCitySource`: today's code, byte for byte, for `classic:*` roster entries.
- `DefinitionCitySource`: for definitions. It builds the static parts **without any RNG**: map
  bytes, cells, districts, properties exactly as realized (sizes, offsets, realtor, status,
  price), actors in Classic id order, suppliers, prices, building names (named-building tiles get
  names in map order, wrapping, as `CityFactory.cpp:119-127`). It then runs the dynamic
  initialisation **in the same order and with the same calls as Classic**: mayor account
  (`:82`), advertising options (`:582`), known arms dealers (`:585-587`), ten economy trend ticks
  (`:589-592`), ingredient trend seeding (`:594`), job market (`:85`). These draw the simulation
  stream exactly when the city is first entered, as in Classic, so an Expanded game is
  deterministic from (game seed, roster, definitions, inputs).

The definition is validated before any live state is touched; an invalid definition never
reaches `DefinitionCitySource`.

---

## 5. Validation rules

Pipeline: stage 1 is JSON Schema (`SCHEMA-1`); if it fails nothing else runs. Stage 2 runs every
rule below and reports all findings with entity refs and tile coordinates. Errors block
`validated`; warnings are recorded in the receipt. A scratch reference implementation was used to
confirm that every fixture fails exactly its listed rule; it is not committed (ownership limits).
The runtime implementation is `src/core/content/LocationReader.cpp` (stage 1: the shape of every
member the runtime reads, with the JSON pointer of the first bad value, and the identity rules
of the schema) and `LocationValidator.cpp` (stage 2); `tests/ContentDefinitionTests.cpp` runs
both over `tests/fixtures/locations/expectations.json`.

| Rule | Sev. | Check |
|---|---|---|
| ID-1 | E | entity ids unique across districts, properties, actors, suppliers |
| ID-2 | E | supplier `number` unique; non-zero phone numbers unique |
| ID-3 | E | `location_key` not `classic:` or `qa-synthetic:` (schema) and no lineage clash in the catalog (catalog time) |
| REF-1 | E | property realtor is a realtor actor |
| REF-2 | E | every district cell < district count |
| TILE-1 | E | no owned shop-front codes (low nibble A, high nibble 0..7) |
| TILE-2 | W | `0xE6`, `0xF6` (road-like codes outside the road rule). `0x77` is allowed: cars cross it straight on, as in the original maps (calibrated, see ROAD-2) |
| TILE-3 | W | `0xB7`, `0x67`, `0x87` (spawn and driving tables disagree) |
| ROAD-1 | E | at least one road tile |
| ROAD-2 | E | lane continuity: no dead end, no U-turn |
| ROAD-3 | E | one connected road network |
| ROAD-4 | E | straight lanes paired with a return lane |
| DIST-1 | E | 1..16 districts |
| DIST-2 | W | every district used by at least one cell |
| DIST-3 | E | population weights not all zero |
| PROP-1 | E | properties and premise tiles in one-to-one correspondence; kind matches the tile |
| PROP-2 | E/W | doorway inside the bottom wall; kitchen door range (E); kitchen height outside the Classic range (W) |
| PROP-3 | E | street access on `street_side` (the "unreachable door" rule) |
| PROP-4 | W | price differs from the Classic formula |
| PROP-5 | E | at least 16 business premises |
| ACT-1 | E | actor role counts (3.5) |
| SUP-1 | E | at least one supplier per food category |
| SUP-2 | W | every valid ingredient sold by some supplier |
| SUP-3 | E | non-zero base price for every valid ingredient |
| NAME-1 | W | named-building types on the map have names |
| ECON-1 | E | at least `min_affordable_premises` premises affordable: for rent with 3 x monthly rent <= reference cash, or for sale with price <= reference cash |
| ECON-2 | E | `price_level <= 10737`, so `price_level x 20 x 20 x 100 x 5` fits in the `int` used at `CityFactory.cpp:730` |
| WORLD-1 | W | geo inside the region's crop bounds (else discoverable from the neighbour only) |
| WORLD-2 | E | bonus placement has null geo |
| ASSET-1 | E | attachment paths contained (schema pattern, re-checked after normalisation) |
| ASSET-2 | E | only `approved` attachments are referenced by a published revision |
| INT-1 | E | admitted documents carry `integrity.definition_sha256` equal to the recomputed hash |

---

## 6. Save contract

### 6.1 Versions

| Document | `version` | New top-level keys | Fingerprint |
|---|---|---|---|
| Classic save written by the new build | **3** (unchanged) | none, except the optional `presentation` block (6.5) | identical to today: `SaveGameManager::fingerprint` (`SaveGameManager.cpp:1228-1249`) hashes the document without `saved_at`, and will also drop `presentation` |
| Expanded save | **4** | `scenario` (6.2), optional `presentation` | hashes everything except `saved_at` and `presentation`, so the roster and definitions are part of the fingerprint |

`SAVE_FORMAT_VERSION` (`SaveGameManager.h:26`) splits into a Classic writer constant (3) and an
Expanded writer constant (4); `migrate_save` (`SaveGameManager.cpp:387-396`) accepts 1..4.
Versions 1..2 keep their migration. An old build refuses a version 4 save because `migrate_save`
rejects versions above its own (`:388-389`): an Expanded save can never be half-loaded by a build
that does not understand it. Absence of `scenario` means the Classic roster.

### 6.2 The `scenario` block (Expanded only)

```json
"scenario": {
  "format": "pthd.save-scenario", "contract_version": "1.0.0",
  "scenario": { "...": "the admitted scenario document, scenario.schema.json" },
  "roster": [
    { "index": 0, "location_key": "classic:paris", "source": "classic", "classic_number": 0,
      "classic_source_sha256": "<hash of dat/city0.pc + map0.dat + bez0.dat, or null>" },
    { "index": 1, "location_key": "pthd:santas-pizza-village", "source": "embedded", "revision": 3,
      "definition_sha256": "<64 hex>", "definition": { "...": "the admitted LocationDefinition" } }
  ]
}
```

All positional arrays that exist today (`city_initialised`, day flags, `weapon_market`,
`turnover_by_city`, `weekly_city_profit`, `days_present`, `player_cities`, `active_city`,
`cities[].id`, `property_city_id`) keep their meaning with length = roster size and index =
roster position.

### 6.3 Embedded definition versus required bundle: decision

**Decision: embed.** Every non-Classic roster entry's admitted definition is embedded in the save,
with its hash. Classic entries are referenced, never embedded (they are original content; the
rebuild from the user's files stays as today). Art is never embedded; it is referenced by asset
id, revision and hash and falls back when missing.

| | Embed (chosen) | Immutable required bundle |
|---|---|---|
| Save opens if the bundle is deleted, renamed, or on another device | yes | no: recovery flow needed |
| Save size | + about 50 KB canonical per location (harbor-bay: 51,566 bytes, 3,385 compressed); budget 256 KiB per definition, 48 locations per game (provisional) | minimal |
| Sharing a save shares the topology | yes (user content only; no original data) | needs the bundle too |
| Duplicate storage across many saves | yes | no |
| Behaviour when the catalog has a newer revision | the game keeps playing its embedded revision; the menu may offer "newer version available" for new games only | must pin and keep every old revision forever |

### 6.4 Migration and load order

Load reads `version`, then (version 4) `scenario`, **before** players and cities, verifies every
embedded hash, migrates embedded definitions in memory if their `schema_version` is older
(never rewriting the embedded bytes until the next save), builds the roster, then proceeds as
today. Expanded loads must use the temporary-state load from W01 (today `json_to_state` resets the
live state first, `SaveGameManager.cpp:786-789`).

### 6.5 The `presentation` block (both modes)

Cosmetic state only: event mode, time zone, per-event toggles, pinned event and cast catalog
`{id, revision, sha256}`, costume assignments `{restaurant_id, job_role, slot, costume_id,
revision}`, reduced-motion flag. It is excluded from the fingerprint, so a test can prove that
toggling any of it never changes the game. It is absent when nothing cosmetic is enabled, so a
default Classic save is byte-identical to today's.

### 6.6 Missing or changed content

| Condition | Detected by | Behaviour |
|---|---|---|
| Classic data missing or different | file missing, or hash differs from `classic_source_sha256` | refuse before touching state; name the city and files; offer "locate game data" |
| Embedded definition damaged | hash mismatch | refuse; "save damaged"; offer the Recovery Center (other autosaves) |
| Embedded definition newer than this build | `schema_version` unknown | refuse; "made with a newer version" |
| Embedded definition older | known older `schema_version` | migrate in memory; notice "will be upgraded when saved" |
| Catalog has a newer revision | key match, revision differs | play the embedded revision; notice only |
| Optional art missing | attachment or asset hash not found | fallback art; notice in the content screen; never an error |
| Costume or event catalog missing | presentation refs unresolved | base appearance, events off; notice; never an error |
| Property or supplier reference unresolved | `json_to_city` lookup | Expanded: refuse with the refs listed. Classic: unchanged warn-and-skip (`SaveGameCity.cpp:686-689,732-734`) |

Budgets: roster <= 48 (provisional, per game), definition <= 256 KiB canonical, save JSON cap unchanged (256 MiB,
`SaveGameManager.cpp:58`). The S21/tablet parse cost must be measured in W07.

---

## 7. Generation contract

Inputs: generator id and semver, template `{id, revision, sha256}`, settings
(`project.schema.json` `generation_settings`: name, region or `bonus:north-pole`, fixed size,
density, road pattern, district count, economic difficulty, starting opportunities, visual
theme, terrain), and a u64 seed as decimal text.

- **Own RNG, never the game's.** The generator uses an instance RNG object (PCG32 XSH-RR, 64-bit
  state). Stage seed = SplitMix64(seed XOR FNV-1a-64(stage id)) for stages terrain, roads,
  districts, premises, actors, suppliers, economy, names, placement. Each stage has its own
  stream, so regenerating one stage does not shift another. It never calls `engine::random`
  (whose two streams are global state, `Random.cpp:14-15`).
- Integer arithmetic only for anything stored; weights come out as integers / 1000.
- Constraints: locked objects, locked layers and manual fields (section 9) are inputs the
  generator must respect; if they cannot be satisfied the job fails with a finding and the
  project is unchanged.
- Output: a LocationDefinition plus a receipt (input hashes, per-stage output hashes,
  definition hash, duration). The output is validated (section 5) before the project accepts it.
- Bounded: per-stage time limit and 32 MiB working memory (phone budget, to be measured); cancel
  at stage boundaries; resumable by job id.
- Version rule: a generator major version bump means the same inputs may give different output;
  old projects keep their realized definitions, so nothing already admitted changes.

Determinism tests: (1) same inputs twice in one process and in a fresh process give the same
`definition_sha256`; (2) Windows x64, Android arm64 and Linux CI give the same hash; (3) running a
generation during a scripted game leaves `engine::random::get_state()` unchanged and the final
`dump fingerprint` equal to a run without generation (the presentation stream has no accessor
today, `Random.h:33-35`, so the test adds one); (4) regenerating one stage with the others locked
leaves their bytes identical; (5) cancelling at every stage leaves the project head unchanged.

---

## 8. Admission states, publication and receipts

| State | Meaning | Receipt that proves it |
|---|---|---|
| draft | any working revision | none |
| validated | stage 1 and stage 2 pass with zero errors | validation receipt (validator version, contract version, findings) |
| integrated-candidate | admitted into a named content version, constructs in an isolated GameState, survives save and fresh-process load | integration receipt |
| gameplay-accepted | the full journey passes on a qualified platform: construct -> enter -> rent or buy -> furnish -> menu -> stock -> staff -> open -> serve -> trading day -> week settlement -> save -> fresh-process reload -> continue and compare fingerprints (and the scenario goal path when the location has presets) | gameplay receipt (engine build, platform, script hash, seed, steps, fingerprints, exact coverage) |
| withdrawn | removed from new games; saves that embed it keep working | none |

**Publication** makes a gameplay-accepted revision the catalog's current playable revision and
needs all three receipts for the same `definition_sha256`. Any edit creates a new draft revision;
the published one stays playable. Receipts are matched by hash, never by revision number alone.
Classic entries have admission `classic`; the stage 0 golden fingerprints (section 16) are their
proof.

**Counting rule.** A new location counts toward the content target only when it is
gameplay-accepted, has a unique key, and keeps the ten Classic identities untouched. Target:
24 regional + 1 bonus = 25 new, 35 with Classic. At the time of writing **0 of 25** exist as
definitions.

---

## 9. Studio project document

Schema: `project.schema.json`; example: `tests/fixtures/locations/valid/harbor-bay.project.json`.

### 9.1 One document, three modes

Quick Create fills `generation` and runs every stage; Guided Create edits `generation.settings`
step by step and may lock what it shows; the Full Editor edits `working.definition` directly.
All three write the same revision history; switching mode changes only `ui`.

### 9.2 Layers

Layer -> parts of the definition: terrain (non-road, non-premise tiles), roads (road tiles),
districts (`districts`, `district_cells`), premises (premise tiles + `properties`), actors,
suppliers (`food_suppliers`, `ingredient_base_prices`), economy (`economy`, district price and
rent levels), names (display, building names, actor and supplier names), placement (`world`,
`facets`), art (`assets`). Each layer records origin and lock state.

### 9.3 Locks and provenance

Locks by layer, object, field or tile rectangle. `working.objects[].manual_fields` marks fields a
person set; regeneration keeps them even without a lock. Locks are inputs to generation, never
hints.

### 9.4 Selective regeneration

A regeneration is one transaction (`kind: regenerate`) with the stages, optional rectangle, seeds
before and after and the list of kept objects, shown to the user before confirming. It produces
a new revision; undo restores the previous one exactly.

### 9.5 Revisions, undo, redo, branches

Revisions form a tree (parent pointers); `head` is the cursor. Undo moves the cursor to the
parent; `history.redo` lists undone revisions until a new edit at head, which clears it (the
abandoned revisions remain for the Recovery Center). Operations are constrained JSON-Patch with
stable selectors (`{kind, id}` or tile rectangles); array-index-only paths are rejected for
entity lists. Each operation stores `old_value`.
Branches cover variations, "Try safely" and restores. Bounded: 2,000 revisions per document;
older ones compact into checkpoints; published revisions never compact.

---

## 10. World catalog and navigation

Schema: `catalog.schema.json`. Real catalog fixture: `tests/fixtures/locations/catalog/world-catalog-v1.json`.

### 10.1 Regions

Eight canonical regions, in this order, with crop bounds taken from
`world-region-art/region-overviews-v1/manifest.json` (equirectangular, degrees west/south/east/north):

| # | Region id | Bounds |
|---|---|---|
| 1 | `north-america` | -170, 10, -50, 83 |
| 2 | `latin-america-caribbean` | -120, -60, -30, 33 |
| 3 | `europe` | -25, 32, 50, 74 |
| 4 | `north-africa-western-asia` | -20, 5, 65, 47 |
| 5 | `sub-saharan-africa` | -20, -36, 55, 23 |
| 6 | `central-south-asia` | 42, -3, 97, 58 |
| 7 | `east-southeast-asia` | 92, -14, 155, 60 |
| 8 | `oceania-pacific` | 108, -52, 180, 8 |

Crops overlap on purpose; they are navigation windows, not geopolitical assignments. A location
belongs to exactly one region by its placement and may also be discovered from a neighbouring
region's view; it is never duplicated or counted twice.

### 10.2 Bonus route

`north-pole` is a bonus route: an illustrated icon on the global overview
(`bonus_routes[].global_marker`), outside every region and country. Its locations have no geo.

### 10.3 Markers

Marker position on a region overview, in basis points:
`x = (lon - west) * 10000 div (east - west)`, `y = (north - lat) * 10000 div (north - south)`, using
the e6 integers. Outside 0..10000 the marker is not drawn in that region (WORLD-1 warns). Each
overview targets 8..12 legible markers: when more would be visible, or labels or touch targets
would overlap, nearby markers merge into a cluster (grid buckets in screen space, members in sort
order); selecting a cluster zooms or opens the list. The full list is always available.

### 10.4 Lists, paging, previous/next

- Page size 12 on desktop and tablet, 6 on a phone (short side below 600 dp; Windows is always
  desktop). `pages = ceil(N / size)`, 0 when N = 0.
- Sort: folded display name (NFKD, marks removed, casefold), then location key. "All locations"
  sorts by region order (bonus last), then name, then key.
- Current page after delete, import or filter: `0` if there are no pages, else
  `min(max(page, 1), pages)`; when a selection exists the page follows it (`index div size + 1`).
  Selection is kept by key; if its entry is gone, select index `min(old, N - 1)`.
- Previous/next move the selection through the filtered order across page boundaries and stop
  at the ends (no wrap); they are disabled with a visible reason at the ends. They are separate
  from map scrolling (in the city view the arrow keys scroll the map).
- Browsing, preview and search never advance time, never travel, never charge money, never
  construct a City and never draw from any RNG stream (1.5). Travel and starting a game are
  explicit actions with the existing confirmation (`EuroMap.cpp:271-341` in Classic).
- Search is offline over the index: display name, aliases, tags, facets. Filters: region or
  bonus route, source, admission state, playable-now, facets. Results are deterministic.

### 10.4a Travel on the Expanded world map (step 9)

A game whose roster is not the Classic ten travels on the Expanded world map
(`src/views/screens/WorldMapView.cpp`, logic in `src/core/content/WorldMap.cpp`); EuroMap stays
for Classic games and game set-up. Regions are the placement's (a bonus route
has its own view); the original ten use the Classic adapter's region and city-centre point
(`ClassicCities.h`, to be replaced by the pinned gazetteer). Fares are integer arithmetic on
micro-degrees and need no RNG:

```
degrees_e6 = min(|dlon|, 360e6 - |dlon|) + |dlat|
base       = degrees_e6 * dollars_per_unit_milli div 1e9         (default 4,000 per degree)
base       = bonus_route_cost_dollars when either place has no geo (default 600)
fare       = base * (difficulty + 1)                              (as EuroMap.cpp)
```

The default rate matches the original map within a dollar (Paris to Berlin: 59 map units
there, 58 here). `scenario.rules.travel` sets both rates. With travel `disabled` the map closes
as it opens and offers no travel. Travel is one explicit action with a confirmation that names
the fare; browsing, paging and previous/next only change the view.

Depot exports of an Expanded game use the same map: the target is chosen the same way, the
cost is the fare above, a place without the player's central depot is refused ("You do not
have a central depot in X."), and with `fix_legacy_bugs` so is a place under embargo, as on
EuroMap. Choosing the current place closes the map; a valid target opens the export screen.

For review captures `nav worldmap [view]` opens the map on a region (id or 0-7) or the bonus
view (`bonus` or 8) for looking only: no travel or export button, open even when travel is
disabled, no time passes. `data/scenarios/review-world-tour.json` (not in any menu) is a game
whose places fill three views: Paris and Madrid (Europe), New York (North America) and the
village (bonus).

### 10.5 Facets

`facets` (definition) and catalog entries carry many-to-many tags: `countries` (ISO 3166-1
alpha-2), `states` (ISO 3166-2), `communities` (reviewed attribution slugs), `biomes`, `settings`
(dense, urban, metropolitan, coastal, inland, hillside, village) and `seasons` (which seasonal art
variants exist; availability is never seasonal). Facets never act as identity, never drive
simulation, and are never a substitute for research. Only codes confirmed by the source documents
are filled in the fixtures; the rest stay empty.

### 10.6 Region kits and user areas

Each region has an art kit `kit:<region-id>` (building shells, facade and shopfront modules,
people and clothing overlays, scenery, signs, effects), versioned and made of admitted assets.
A definition references a kit; the project's art layer can swap it or override single bindings,
so user-made areas stay editable. Kits are presentation only: a new picture does not create a new
simulated business type.

### 10.7 Overview and menu art

Overview backdrops come from geodata renders (section 15). Generated art may decorate but is
drawn under labels and never used for hit testing.

### 10.8 Content target

All new entries are plans (`content_plan` in the catalog fixture), not catalog entries. Keys reuse
the art candidate ids from `world-region-art/area-catalog-v1.json`.

| Region | Classic (admission `classic`) | Planned new locations (`pthd:` keys) |
|---|---|---|
| north-america | Baltimore, Chicago, New York | toronto-ontario-canada, victoria-british-columbia-canada, santa-fe-new-mexico-united-states |
| latin-america-caribbean | | mexico-city-mexico, valparaiso-chile, mendoza-argentina |
| europe | Paris, Berlin, Vienna, Madrid, Rome, Stockholm, London | rotterdam-netherlands, split-croatia, lyon-france |
| north-africa-western-asia | | amman-jordan, muscat-oman, tunis-tunisia |
| sub-saharan-africa | | accra-ghana, mombasa-kenya, kigali-rwanda |
| central-south-asia | | almaty-kazakhstan, kochi-kerala-india, kathmandu-nepal |
| east-southeast-asia | | seoul-south-korea, da-nang-vietnam, chiang-mai-thailand |
| oceania-pacific | | melbourne-victoria-australia, suva-fiji, alice-springs-northern-territory-australia |
| bonus `north-pole` | | santas-pizza-village |

10 + 24 + 1 = 35. Each planned area needs a definition, its business story translated into
registry objectives (section 11) and the section 8 receipts before it counts. The product name
stays Pizza Tycoon HD everywhere.

### 10.9 Catalog QA fixtures (UI only)

`tests/fixtures/locations/catalog-qa/`: synthetic catalogs of 35 (`[6,3,10,3,3,3,3,3]` + 1
bonus), 64, 96 and 1,000 entries, plus `[0,1,13,0,25,6,7,2]` with empty regions and partial last
pages; `expected-pages.json` (per-region and all-locations page counts, last-page sizes, first
and last key) and `page-math-cases.json` (exact fit, one over, delete on last page, delete
before selection, filter to zero, filter shrink, import growth, 1,000-entry phone and desktop
cases). Selected values: 1,000 entries -> 11 desktop / 21 phone pages per region, 84 / 167 for
All locations; 64 -> 6 / 11; 96 -> 8 / 16; 35 -> Europe 1 / 2, All 3 / 6. Every entry has a
`qa-synthetic:` key and every catalog has `ui_only: true`. A fast 1,000-entry list proves nothing
about 1,000 playable maps.

---

## 11. Scenarios, milestones and objectives

Schema: `scenario.schema.json`.

### 11.1 Scoping

- **Classic:** unchanged. City milestones 100-119 by city number, `WinCondition` Paris / AnyCity /
  AllCities (`src/core/state/GameRules.h:18`, `MilestoneTracker.cpp:206-254`), threshold 5
  (`:215`), warehouse 5/10 (`:318-331`), western cities = 10 (`:312-315`), missions as today.
- **Expanded:** milestones 100-119 fire only for roster entries with a classic number, using that
  number (rows 24-25 in 1.2). Other locations get no city-numbered milestone in 1.0 (a generic
  "arrival in a new location" needs new text and a location-scoped dedupe key: milestones are
  deduplicated per id and player game-wide, `GameState.cpp:488-492`). "All cities" means all
  roster locations. The win rule comes from the scenario (`none` or `market_share` with a scope
  and percent). Mission mode stays Classic-only in 1.0.
- City-scoped milestones (market share 37-39, `RepresentedInAllAreas`) are not tagged with a city;
  a scenario that uses them as objectives must have a single-location roster (SCN-4).

Scenario rules (stage 2 after `scenario.schema.json`, `src/core/content/Scenario.cpp`): SCN-1 a
location appears once and is pinned `classic` exactly when it is one of the original ten; SCN-2
the start (and every listed win location) is in the roster; SCN-3 `continue-sandbox` needs win
mode `none`; SCN-4 city-scoped milestones need a single location; SCN-5 travel `disabled` needs a
single location. At game creation: SCN-6 every non-Classic entry has a valid definition, equal to
its pin when the pin is exact; SCN-7 the roster fits the per-game limit of the build (10 while the
per-city arrays keep `NUMBER_OF_CITIES` slots).

### 11.2 Objective registry v1

Objectives are MilestoneIds (`MilestoneId.h`) with a verified emitter. Polarity `achieve` is met
when the milestone is recorded for the human player; `avoid` is met while it never has been.

| Milestone (id) | Emitted at |
|---|---|
| OpenedFirstRestaurant (30), OpensSecondRestaurant (31) | `MilestoneTracker.cpp:292`, `:295` |
| HasFiveRestaurants (34) | `MilestoneTracker.cpp:282` |
| Sold1KPizzasPerDay (35) | `MilestoneTracker.cpp:127` |
| Achieved10/20/50PctMarketShareInCity (37/38/39) | `MilestoneTracker.cpp:201`, `:198`, `:194` (the 10% one fires at 5% unless `fix_legacy_bugs`, `:191-192`) |
| AwardedFirstStar (40), FirstTwoStarRestaurant (41), FirstThreeStarRestaurant (42) | `TurnOrchestrator.cpp:590`, `:593`, `:596` |
| AchievedTurnover10KPerDay (45) | `MilestoneTracker.cpp:133` |
| PizzaReachedNo1InCharts (47) | `TurnOrchestrator.cpp:1493` |
| AbleToMeet100PctDeliveries (48) | `MilestoneTracker.cpp:114` (a week with no lost orders, `:42-53`) |
| AverageServiceTimeLessThan4Minutes (49) | `MilestoneTracker.cpp:108` |
| HasAssetsOf100K (52), YouHaveAssetsOf500K (53), YouHaveBecomeMillionaire (54) | `MilestoneTracker.cpp:65`, `:68`, `:71` |
| FullyQualifiedWaiter (67), FullyTrainedMasterchef (66) | returned by `master_milestone`, `Restaurant.cpp:935`, `:942` |
| FirstPriceInPizzaCompetition (87) | `CompetitionCoordinator.cpp:261` |
| Has25KWeeksProfitInOneRestaurant (121) | `MilestoneTracker.cpp:167` |
| Has1000CustomersPerDayInOneRestaurant (133) | `MilestoneTracker.cpp:103` |
| PerfectDeliveriesFor1WeekAcross5Restaurants (134) | `MilestoneTracker.cpp:120` |
| RepresentedInAllAreas (135) | `MilestoneTracker.cpp:151` |
| avoid: TakesOutFirstLoan (32) | `BankConversation.cpp:139` |
| avoid: FinancialDifficultiesFirstTime (33) | `Player.cpp:267` |

Per-restaurant daily milestones look only at the active city's restaurants
(`MilestoneTracker.cpp:91-93`); in a single-location scenario that is always the scenario city.

### 11.3 Ending versus continuing

Today both endings remove the player: mission completion calls `eliminate_player(...,
MissionComplete)` (`GameState.cpp:519-531`), and the win sequence ends with
`eliminate_player(..., PizzaTycoon)` (`WinSequenceView.cpp:97`). A preset with
`after_goal: continue-sandbox` therefore needs a new observer that watches recorded milestones,
shows the victory summary once, records "goal reached" in the save, and returns to normal play.
It must not reuse either ending, and the scenario must set `win: {mode: none}` (SCN-3): with one
location, `AnyCity` would otherwise end the game at 5% market share (`MilestoneTracker.cpp:227-238`).

### 11.4 Santa's Pizza Village presets

Roster `[pthd:santas-pizza-village]`, travel disabled, win none, continue as sandbox, always
available, no date lock. Fixtures: `tests/fixtures/locations/scenarios/`.

| Preset | Start | Difficulty | Goal (all of) | Optional challenges |
|---|---|---|---|---|
| Cozy Village (forgiving) | quick start, big (`GameState.cpp:1119-1134`) | 1 PUSHOVER | HasAssetsOf100K; AwardedFirstStar | AverageServiceTimeLessThan4Minutes; avoid TakesOutFirstLoan |
| Workshop Rush (tighter service and stock) | free mode | 3 NORMAL | AbleToMeet100PctDeliveries; AverageServiceTimeLessThan4Minutes; Sold1KPizzasPerDay | Has1000CustomersPerDayInOneRestaurant; avoid FinancialDifficultiesFirstTime |
| Pizza Empire (longer expansion) | free mode | 3 NORMAL | HasFiveRestaurants; Achieved20PctMarketShareInCity; YouHaveAssetsOf500K | RepresentedInAllAreas; FirstThreeStarRestaurant |

| Sandbox (no goals) | free mode | 2 EASY | none: an empty goal never announces and can only continue (SCN-3) | none |

Shipped in `data/scenarios/` (`north-pole-*.json`, and the review game `review-world-tour.json`),
all pinned to revision 4 of
`data/locations/north-pole/santas-pizza-village.json`, whose six districts follow
NORTH-POLE-FLAGSHIP.md: Village Square (business, centre), Candy Cane Market (shopping),
Toy Workshop Quarter (industrial), Reindeer Ridge (leisure), Frosty's Snow Gardens
(residential, with the depot) and Aurora Harbor (the outskirts and the frozen harbour), each with
premises on the one road network. Christmas decoration is a suggestion of the presentation block,
switchable and never date-locked (`availability.calendar_lock` is `false`).

District identity (revision 3). Until each district has its own tile atlas, each reads
differently with the original landsym tiles: shopfronts and lanes of market stalls in Candy
Cane Market, long red workshop roofs with yards and shadowed lanes along the Toy Workshop
Quarter, a paved plaza with fountains and zebra crossings at Village Square, meadow, trees,
ponds and a hay field on Reindeer Ridge, pairs of houses on lawns with garden rows in Frosty's
Snow Gardens, and a quay and frozen harbour in Aurora Harbor. Properties, districts and the
road network are those of revision 2; only tiles changed. A test checks that each district's
most common tile, leaving out grass, roads and buildings with a role, is its own. The
per-district atlas key comes with the art. Revision 4 changes only the asset keys, to the
location's own art in the classic layout: `north-pole:gfx/landsym` and `north-pole:gfx/med1`
(`<namespace>:gfx/<picture>`, where the picture is one the original game has: landsym, med1, k*,
back*; views/hd/LocationArt looks for it under `locations/<namespace>/` in the HD pack).

Difficulty acts through the existing levels (`NewGameConfig.h:27-33`), for example supplier price
factors 0.6..1.9 (`docs/blueprint/content-data.md` 3.2). The village definition must allow these
goals: at least 16 business premises (PROP-5) with five affordable at Pizza Empire difficulty, and
at least four districts. Balance values are first guesses to be tuned in playtests.

---

## 12. Optional calendar events

Schema: `event-catalog.schema.json`; fixtures and resolution oracle in `tests/fixtures/locations/events/`.

- **Modes:** `off`; `in-game-calendar` (the game date, `GameState::time().current_date()`, the same
  clock that already drives the city season palette, `CityView.cpp:177-189`); `real-date`, opt-in,
  using an IANA time zone chosen by the player and the local date (local midnight, not UTC). A
  manual preview shows one event without changing settings, game date, save or any RNG, and is
  cleared on exit. Defaults: Classic off, Expanded in-game calendar.
- **Resolution** (pure function, no clock read in in-game mode, no RNG, no network): an event is
  active when enabled, applicable to the location (everywhere, or listed keys, regions, bonus
  routes, countries; optional hemisphere), and the date lies within `[d - before, d + after]` for
  the rule date d in the previous, current or next year. `date-table` rules list sourced dates;
  outside the listed years the event is inactive and nothing is extrapolated. Order: priority
  descending, then event id. One overlay per slot; the higher priority wins.
- **Isolation:** overlays join presentation layers only (city building and scenery, restaurant
  decor, people costumes, UI frame, menu backdrop, effects). Event code has no write access to
  game state. Overlays can never change economy state, collisions, room access, ownership, prices,
  stock or RNG draws. Test: a scripted run with events on, off and previewed gives the same
  `dump fingerprint`.
- **Accessibility:** every animated or flashing overlay has a static replacement (EVT-4); reduced
  motion uses it; event audio has its own toggle, off by default; secular or neutral alternatives
  are offered beside religious ones; no event is mandatory and none has a gameplay effect.
- **Versioning:** the catalog has an id and revision; the save pins `{id, revision, sha256}` in
  `presentation`. Missing overlays fall back to no overlay. No date can lock a location, scenario
  or save (`scenario.availability.calendar_lock` is `false` by schema).

---

## 13. Main menu data

The menu UI is not in this contract; this is the data it reads.

- Today the menu has Old Game, New Game, High Scores, Options, Exit (`src/views/screens/menus/MenuMain.cpp:24-53`).
  Target: Continue, New (Classic or Expanded), Load, Studio, High Scores, Options, Exit.
- **Continue** is shown only for the latest save that passes a resumable check: header and
  metadata parse, version 1..4, and (version 4) embedded hashes verify and Classic data for the
  roster is present. It never loads anything into live state just to decide visibility.
- **New:** Classic starts exactly as today; Expanded lists scenarios whose roster entries are all
  `classic` or gameplay-accepted.
- **First-open gates:** original game data located (`src/engine/Game.cpp:226-242`; Classic and
  every Classic roster entry need it); catalog index loads and validates (otherwise the menu
  offers Classic only and a repair message); optional backdrop art present (otherwise the
  original `gfx/menu`).
- **Region teasers** (`catalog.menu.teasers`): one per region plus the bonus route, up to three
  featured keys (classic or gameplay-accepted only), an admitted backdrop or null.
- **Preview route:** menu -> world overview -> region overview -> location preview, all
  read-only: no City is built, no GameState is touched, no RNG is drawn.
- **Visibility:** Play needs admission `classic` or `gameplay-accepted`; Edit and Validate need an
  editable entry (drafts show Edit and Validate only); Classic entries are never editable.

---

## 14. Cast and costumes

Schema: `cast-catalog.schema.json`; fixture: `tests/fixtures/locations/cast/holiday-cast.cast-catalog.json`.

### 14.1 How people are drawn today

| Role | Sprite choice today | Source |
|---|---|---|
| Customer walking | demographic weighted by the restaurant's customer counts, `guest_type = demographic + (coin ? 6 : 0)`, sheet `bob/walk{guest_type}.bob`; all draws from the presentation stream | `GuestPicker.cpp:14-37`; `GuestWalkScene.cpp:280-296,487-491` |
| Customer seated | 7 body parts from `bob/gaeste.bob`, frame = part base + facing + guest_type x 4 | `CharacterRenderer.cpp:13-44,134-155` |
| Customer face | `gfx/g{guest_type x 2 + coin}` with an eye-blink overlay | `GuestPicker.cpp:39-41`; `GuestTalkPopup.cpp:202-213` |
| Waiter walking | fixed sheet `bob/walk12.bob` | `GuestWalkScene.cpp:303-321` |
| Chef, manager | not drawn in the room; portrait only | `RestaurantView.cpp:382-385` counts cooks; no chef sprite |
| Staff portrait | the character's 59 x 52 portrait from `chr/cNe.pc` | `docs/blueprint/file-formats.md` 1.11; `Character.h:47,131`; `StaffView.cpp:982,1142` |
| Actors (mayor, bank, ...) | `bob/typ{head}e.bob`, head 1..25 | `Actor.cpp:71-75`; `ActorView.inl:28-36,285-300` |

The engine has no host job (`JobRole` is CHEF, WAITER, MANAGER, NONE, `Character.h:26-31`) and no
decorative people layer. In this contract `host` means a presenter portrait on existing dialog and
alert surfaces (`TurnPresenterView.h:23` takes a portrait), and `decorative` means a new overlay
layer with no collision and no navigation effect.

### 14.2 Frame contracts

| Contract | Frames | Index rule | Anchor | Fallback sheet |
|---|---|---|---|---|
| walker-12 | 12 | facing x 3 + phase; facing S, W, N, E; cycle 0, 1, 0, 2 (`Walk.h:18,43-45,66-68`) | frame 0 centre + (10, 16) (`GuestWalkScene.cpp:378-384`) | `bob/walk12.bob` or `bob/walk{guest_type}.bob` |
| seated-parts-28 | 28 (7 parts x 4 facings) | part base 0x00 .. 0x120 + facing + type x 4 | per part and facing (`CharacterRenderer.cpp:134-155`) | `bob/gaeste.bob` |
| guest-face | 1 picture | `gfx/g{n}` | popup face position, eye sub-rectangle | `gfx/g{n}` |
| staff-portrait | 1 (59 x 52) | portrait | card frame offset | the wearer's own portrait |
| actor-head | at least 6 (actor states, blinking eyes = frame 5, `Actor.h:38`) | state | eye table offset | `bob/typ{head}e.bob` |
| decorative-loop | 8 | loop; frame 0 when reduced motion | bottom centre on a tile | none (omitted) |

Alpha: indexed frames use palette index 0 as transparent (`TRANSPARENT = 0`, `Surface.h:24`;
`blit_masked`, `:77-78`); HD frames are PNG with straight alpha through the existing HD pack frame
lookup (`HdPack.h:66`).

### 14.3 The 20 entries

16 characters and 4 costumes; ids `holiday:<slug>` reuse `world-region-art/holiday-cast-catalog-v1.json`.

| Entry | Kind | Roles (contracts) |
|---|---|---|
| santa, mrs-claus, nutcracker-host | character | host (staff-portrait), decorative (decorative-loop) |
| rudolph, frosty | character | decorative, host |
| elf-chef, gingerbread-baker | character | staff:chef (staff-portrait) |
| elf-courier | character | staff:waiter (walker-12, staff-portrait) |
| dasher, dancer, prancer, vixen, comet, cupid, donner, blitzen | character (reindeer) | decorative |
| winter-chef-costume | costume | staff:chef (staff-portrait) |
| winter-server-costume | costume | staff:waiter (walker-12, staff-portrait) |
| winter-delivery-costume | costume | staff:waiter (walker-12); deliveries are not drawn today |
| caroler-host-costume | costume | customer guest type 3 (walker-12, seated-parts-28, guest-face), host |

Reindeer are cosmetic only: no movement, sled or flight behaviour. Santa as the village's mayor
would need the head tables extended (`ActorView.inl:28-36`) and is not in 1.0. Concept status was
read from batch `20261114T183935Z` while it was running (18 generated, Donner running, Blitzen not
started); no runtime frame exists for any entry.

### 14.4 Rules

- **Appearance only:** a character bound to staff is a skin over the existing Staff record
  (`SaveGameRestaurant.cpp:32-43` saves the role and character); costumes and characters carry
  `economics: none` and never change skills, wages, popularity or any value.
- **No new RNG draws:** costume choice for customers follows the guest type already drawn;
  enabling costumes adds no draw.
- **Never invisible, never a crash:** today a BOB that cannot be read is fatal
  (`BobLoader.cpp:25-27`) and an out-of-range frame asserts (`Bob.cpp:38`). A costume resolver
  checks each costume sheet's existence and frame count against its contract at admission and at
  load, before any `BobManager::get`, and falls back per frame: costume frame -> base sheet frame
  with the same index -> the wearer without costume. Only decorative overlays may be omitted.
- **Saves:** costume assignments live in the `presentation` block (6.5) with catalog revision and
  hash; unresolved references fall back and never block a load.

---

## 15. Geography, art and asset admission

- **Geodata is authoritative** for coastlines, region crops and coordinates. Current source:
  Natural Earth `ne_110m_land.geojson`, public domain, sha256
  `9e0729ee253ca7d7a5c4ae9395fb1902264c5377c52e224d13dd85010e2835d9`, fetched from the
  `nvkelso/natural-earth-vector` master branch (`world-region-art/menu-concept-v1/provenance.json`).
  It must be pinned to a release before shipping (CAT-3). City coordinates need a pinned gazetteer
  (open question 3); until then `geo` stays null and markers are not drawn.
- **Generated art is decorative only.** It never defines coastlines, coordinates, tiles,
  collisions or prices. The 24 location scenes (batch `20261114T183013Z`) are generic modern
  references, not region-authentic art and not playable maps. The first regional atlas attempt
  (`20261114T182320Z`) is not selected (medieval-style failure noted in `ASSET-INDEX.json`).
- **Admission record** (`asset-admission.schema.json`): states candidate -> curated ->
  style-reviewed -> runtime-validated -> admitted (or rejected). Each transition appends a review
  entry with notes and checks. Provenance records source type, model, seed, prompt and prompt id,
  workflow file and hash, batch receipt, hardware or geodata source. Native size and hash are
  kept separately from derived files (resizes, crops, palette conversions, frame cuts), each with
  its method; a resize is never extra authored detail. Unknown rights block runtime-validated and
  admitted; a generic modern reference cannot be admitted as location art. Only admitted assets
  are displayed.
- **Runtime budgets** (to measure in W07): per-asset decoded byte limit, phone mip levels, never
  decode every regional 4K master at once.

---

## 16. Refactor order (runtime code)

Each step lists the code and the test that proves Classic is unchanged. "Golden" means the stage 0
fingerprint suite: scripted runs with `--seed` (`src/Main.cpp:115`) and `--exec`, ending in
`dump fingerprint` (`DebugCommands.cpp:184`, `SaveGameManager.cpp:1228-1249`), as in
`tools/ci/hd_gates.py:87-100`, covering each of the 10 cities, quick start, a mission, month and
year rolls, AI expansion, newspaper events, mafia and police, plus loading v1, v2 and v3 saves.

| # | Change | Code | Proof that Classic is unchanged |
|---|---|---|---|
| 0 | Golden fingerprint suite and legacy save fixtures, recorded before any edit | `tests/golden/` (new), `tools/ci` | baseline hashes stored |
| 1 | `LocationKey`, Classic adapter table, roster type (new files only) | new `src/core/content/` | unit test: 0..9 <-> keys bijection; names equal `CityNames.h:19-20` |
| 2 | Roster in GameState; arrays become roster-sized | items 2-4, 10-15, 18-19 in 1.1 | golden unchanged; Classic save JSON byte-identical; weapon market still 40 slots |
| 3 | Bounds and loops from the roster | items 5-9, 17, 20-23 | golden unchanged; `armsdealer market` output unchanged; same draw at `MafiaConversation.cpp:351` |
| 4 | City name from the roster | `CityNames.h:18-28`; `EuroMap.cpp:82,94,136,142,176` | golden; EuroMap interaction ids still `euromap.Paris` etc. |
| 5 | `CitySource` dispatch; `DefinitionCitySource` | `CityFactory.cpp:70-87` | golden; per-city property list hash equals a pre-refactor dump |
| 6 | Milestones and win rule through the roster | `GameState.cpp:910-913`; `MilestoneTracker.cpp:183-184,219-220,240-242,312-315` | golden runs that reach arrival, domination, win and mission end |
| 7 | Save v4 and the `scenario` block; Expanded hard errors on unresolved refs | `SaveGameManager.h:26`; `SaveGameManager.cpp:387-396,562,884-907`; `SaveGameCity.cpp:686-689,732-734` | Classic saves byte-identical; v1-v3 fixtures load with unchanged fingerprints |
| 8 | `presentation` block, excluded from the fingerprint | `SaveGameManager.cpp:1228-1231` | golden unchanged with nothing enabled; equal fingerprints with events or costumes on |
| 9 | Expanded world map (region overviews); EuroMap stays for Classic | new view; `EuroMap.cpp` untouched | golden EuroMap travel script (Paris to Berlin costs 59 x (difficulty + 1)) |
| 10 | Catalog, browser and menu data (UI only) | new; `MenuMain.cpp:24-53` (UI owner) | browsing script then `dump fingerprint` equals no-browse run; `engine::random::get_state()` unchanged |
| 11 | Scenario goal observer and `win: none` | new; must not touch `GameState.cpp:519-531`, `WinSequenceView.cpp:97` | golden mission and win runs unchanged |
| 12 | Costume resolver before any costume sheet load | `GuestWalkScene.cpp:382,489`; `CharacterRenderer.cpp:14`; `BobLoader.cpp:25-27` | with no costume configured the code path is identical (golden + HD equivalence gate) |
| 13 | Event resolver (pure) | new | events on and off give equal fingerprints; RNG states unchanged |
| 14 | Generator library and tool (own RNG) | new | determinism tests in section 7 |
| 15 | Later, by its own contract: map size | items in 1.4, starting with `Car.cpp:23` and `RestaurantEconomy.cpp:202,204` | golden |

Status (W03, 2026-11-14): steps 0-2 landed; steps 3-6 and 11 are written in `src/core/content/`,
`GameState`, `MilestoneTracker`, `CityFactory` and the console bounds, with the per-city arrays
kept at `NUMBER_OF_CITIES` (a roster holds at most that many places). Step 7 is specified as a
diff for the save owner (`ScenarioSave.h` holds the block reader and writer). The goal observer
reads the career log only; the victory message is a Maestro message, so neither ending is reused,
and `GameState::finish_win_sequence` keeps the Classic ending for the win sequence.

---

## 17. Unrelated projects

The Barros Pizza Studio kit and the Pizza Connection 3 tools on the owner's PC are separate
projects. Nothing from them is used here: no code, art, schemas or adapters (owner's decision,
2026-11-14). An earlier draft of this contract compared ideas with that kit; those references
were removed. Project ids use the pattern `^[a-z0-9][a-z0-9._-]{2,127}$`; location keys use the
namespaced pattern above.

---

## 18. Defects found while verifying (not fixed, Classic must keep them)

1. `CityFactory.cpp:535` tests `mayor_id` instead of `!mayor_id`, so the mayor id is never set and
   `:572` dereferences an empty `std::optional`. Other code finds the mayor by type
   (`GameState.cpp:807`). Definitions name the mayor explicitly.
2. `City::district_index_by_index` returns `uint8_t` for a cell index up to 299
   (`City.cpp:517-521`), so premises in cells 256..299 (the bottom-right of the map) read the
   district of cell `index - 256` when `populate_properties` draws their size
   (`CityFactory.cpp:697-703`). Part of the Classic fingerprint; definitions store realized sizes.
3. The door offset is drawn from the interior height (`CityFactory.cpp:717`) but placed along the
   bottom wall, whose length is the width (`WalkNavigation.cpp:245-247`); with `h > w + 2` the
   doorway can extend past the wall. Not verified at runtime.
4. The second clause of `is_business_premise_by_type` (`City.cpp:132`) can never be true because
   named buildings are excluded first (`:125-127`).
5. Three road codes spawn cars in one direction and drive them in another (3.2).
6. A missing BOB file is fatal (`BobLoader.cpp:25-27`): relevant to any new optional art.

---

## 19. Owner decisions (2026-11-14) and what is still open

Decided:

1. **Geography:** use the Natural Earth geometry already pinned with its provenance in
   `world-region-art/region-overviews-v1` (the overview manifest records the exact source
   revision and hashes). Never "latest". City coordinates come from the same pinned source.
2. **Rosters:** Expanded games may mix the preserved Classic cities with new areas (the full
   target is 35 combined). The roster is frozen per game.
3. **Classic stays visually faithful.** Cosmetic events and costumes are opt-in Expanded
   features with a preview and an off switch.
4. **Missions:** legacy Classic missions are preserved. New locations still get their own
   scenarios and goals built on the supported economy (section 11).
5. **Borders:** every place has one primary browser region plus searchable secondary geography
   tags; Mexico stays in Latin America and the Caribbean.
6. **No catalog ceiling.** The per-game roster limit above is a provisional, measured
   active-simulation budget, kept separate from the catalog; the S21 budget needs real device
   evidence.

Still open:

7. Balance thresholds to calibrate against Classic data dumped locally: PROP-5 (16 premises),
   ECON-1 (3 x rent), population weight magnitudes, and whether ROAD-4 holds for the Classic maps.
   Measured so far (local dump of the original data, nothing copied): `0x77` is a pass-through
   junction (ROAD-2 and TILE-2 changed accordingly); PROP-3 is stricter than Classic, where about
   half of the premises face no road; ROAD-4 holds in most original cities; the eight player
   characters start with $3,000 to $20,000 (character 0: $8,000), which is the reference cash of
   Santa's Pizza Village; original district rents are 5..70 per tile (median 24) and prices
   200..6,500 (median 2,000).
8. Names are limited to ISO-8859-1 by the original fonts; accept for 1.0, or require HD text for
   names outside it (for example scripts for Seoul or Kathmandu signage)?
9. Event catalog content: which observances, with which local review, beyond the fixture's
   Christmas, winter lights and New Year?
