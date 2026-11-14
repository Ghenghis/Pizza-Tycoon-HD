# Modding SDK and Expanded mode: roadmap

A staged plan, derived only from what the code does today ([README](README.md) and the other
blueprint files). Every refactor names the code it touches.

## Ground rules

1. **Classic stays the 1994 game.** A Classic game must produce the same game state, step for
   step, as today: same RNG draws in the same order, same rules, same content, same save
   fingerprint. All expansion lives in a separate mode that is chosen when a game is created
   and stored in the save.
2. **No original assets in mods or in the repository.** Mods ship their own art, text and
   data. Tools that read the user's game copy write their output locally only.
3. **Data before code.** Each stage first moves a hard-coded table into data with the Classic
   values reproduced exactly, then lets mods extend it.
4. **One way to prove it.** Every stage ends with the Classic fingerprint tests (stage 0)
   unchanged.

## Current state in one table

| Area | Data-driven today | Hard-coded today | Key code |
|---|---|---|---|
| Furniture | all values (`data/furniture.json`) | the entry set (`FurnitureType` enum), sprite frames, light sources by name | `FurnitureFactory`, `data_file::load`, `light_profile` |
| Ingredients | values (`data/ingredients.json`) | ids, names, categories, counts | `IngredientType.h`, `FoodSupplier.h` |
| Equipment vendors | values (`data/suppliers.json`) | the entry set (`SupplierID`) | `SupplierFactory` |
| Trainings | values (`data/trainings.json`) | the entry set, 6 per job | `TrainingFactory` |
| HD art | any picture or frame can be replaced (HD pack) | no new pictures or frames | `HdPack` |
| HD text | `data/fonts/hd_fonts.json` | font list | `HdTypefaces` |
| Cities | from original binary files | count (10), names, map size, Euro map positions, milestones | `CityFactory`, `Constants.h`, `CityNames.h`, `EuroMap.cpp` |
| Recipes | original and PREZ `.pc` files | 14 standard pizzas, names, cook book | `PizzaFactory`, `StandardPizza.h` |
| Restaurant layouts | original `.pc` files | AI layout choice formula, counts | `RestaurantLayoutFactory`, `RealEstate.cpp` |
| Newspaper events | article text in original files | 50 events and their effects | `NewspaperEventSystem.cpp` |
| Missions | `.mis` files (original format + PLRS trailer) | milestone set | `MissionFile.cpp`, `MilestoneId.h` |
| Commercials, posters | | all | `CommercialFactory.cpp`, `AdvertisingCampaign.h` |
| Palettes per picture | | all | `AssetMetadata.cpp` |

## Stage 0: Classic guard rails (no gameplay change)

Goal: a regression net before any refactor.

* **Golden fingerprint scripts.** Extend the approach of `tools/ci/hd_gates.py`
  (`gate_fingerprint`): run the game with `--seed <n> --exec <script>`, play a fixed
  sequence (`restaurant qs`, `nav ...`, `game tick N`, `game turn end`), and write
  `dump fingerprint <file>` (`SaveGameManager::fingerprint`). Store the expected hashes per
  script and seed in `tests/golden/` and fail when they change. Cover: each of the 10 cities,
  quick start, a mission start, month and year boundaries, a monthly competition, AI
  expansion, newspaper events (`news schedule fire`), mafia and police commands.
* **Unit tests for the formats** in `tests/` (Catch2 target `test_plegacy`): VGA RLE round
  trip (`VgaLoader` / `VgaWriter`), BOB offsets, `.pc` and PREZ round trip
  (`read_pc_file` / `save_prez`), `.mis` with and without trailer, `kachinfo.pc` parsing.
  Tests must not need the original game; build synthetic files.
* **Data validation tool** `tools/plmod/validate.py` (Python, run with `python3 -I`, same style
  as `tools/hdpack/hdpack.py validate`): checks `data/*.json` against JSON Schemas (stage 1
  schemas), cross-references (furniture `supplier` exists, `bobs` frame indices exist in the
  exported manifest from `--export-assets`), and reports what the engine would reject.

Exit criterion: the golden scripts are in the local CI (`scripts/ci-local.*`).

## Stage 1: content registries with stable string ids

Problem: `data_file::load<T, E>()` (`src/core/factories/DataFile.h`) requires the JSON keys to
be exactly the values of a C++ enum. No entry can be added without recompiling.

Refactor:

1. Introduce `content::Registry<T>` (new, e.g. `src/core/content/Registry.h`): entries keyed by
   a string id (`"classic:TRADITIO_ROMANO"`), each with a dense runtime index and an optional
   **classic number** (the current enum value). Classic files and saves keep using the
   classic number.
2. Port, one at a time, keeping the enums as named constants for the Classic code that refers
   to specific entries:
   * `FurnitureFactory::create_furniture`, `all_furniture`, `furniture_with_role`
     (`src/core/factories/FurnitureFactory.cpp`). Layout files map their byte through the
     classic number (`RestaurantLayoutFactory::create_layout`).
   * `IngredientFactory::create_ingredient` (`IngredientFactory.cpp`); names move from
     `to_string(IngredientType)` into the JSON (`name` field), Classic names unchanged.
   * `SupplierFactory` (`SupplierFactory.cpp`) and `FloorPatternFactory::to_supplier_id`.
   * `TrainingFactory` (`TrainingFactory.cpp`); the course level becomes a `level` field
     instead of the enum position.
3. Load order: `data/` (classic base, shipped) then each enabled mod's `content/` folder.
   A mod may add entries and (Expanded only) override values. Classic games load the base
   only.
4. Saves: furniture, ingredients and recipes are stored by number today
   (`SaveGameRestaurant.cpp` `furniture_type`, `SaveGameFood.cpp` `ingredient_id`). Add
   string ids for non-classic entries and a top-level `content` block (mod ids, versions,
   content hash). Bump `SAVE_FORMAT_VERSION` (`SaveGameManager.h`) and add a migration in
   `migrate_save` that fills the block with `classic` for old saves.

Proposed furniture schema (JSON Schema, draft 2020-12, abridged):

```json
{
  "$id": "pizzatycoon/furniture.schema.json",
  "type": "object",
  "additionalProperties": {
    "type": "object",
    "required": ["name", "category", "supplier", "price", "quality", "capacity",
                 "bake_time", "placement_layer", "popularity", "sprite"],
    "properties": {
      "classic_id": {"type": "integer", "minimum": 0, "maximum": 255},
      "name": {"type": "string", "maxLength": 32},
      "category": {"enum": ["OTHER", "TABLE", "CHAIR", "DECOR", "ENTERTAINMENT", "GAMES",
                             "SLOT_MACHINES", "UNKNOWN", "KITCHENWARE", "UNKNOWN3"]},
      "supplier": {"type": "string", "description": "vendor id"},
      "price": {"type": "integer", "minimum": 0},
      "quality": {"type": "integer", "minimum": 0, "maximum": 100},
      "capacity": {"type": "integer", "minimum": 0},
      "bake_time": {"type": "integer", "minimum": 0},
      "placement_layer": {"enum": ["Seating", "Tables", "Top"]},
      "popularity": {"type": "array", "items": {"type": "integer"}, "minItems": 6, "maxItems": 6},
      "role": {"enum": ["None", "AshesSmall", "AshesLarge", "AshesKitchen", "Jukebox", "Tv"]},
      "sprite": {
        "oneOf": [
          {"type": "object", "required": ["classic_bobs"],
           "properties": {"classic_bobs": {"type": "array", "minItems": 4, "maxItems": 4}}},
          {"type": "object", "required": ["frames", "palette"],
           "properties": {
             "frames": {"type": "array", "minItems": 4, "maxItems": 4,
                        "items": {"type": "string", "description": "indexed PNG path in the mod"}},
             "palette": {"type": "integer", "minimum": 0, "maximum": 70},
             "hd_frames": {"type": "array", "items": {"type": "string"}}
           }}
        ]
      },
      "light": {
        "type": "object",
        "properties": {"radius": {"type": "number"}, "rgb": {"type": "array", "minItems": 3, "maxItems": 3},
                       "intensity": {"type": "number"},
                       "flicker": {"enum": ["Steady", "Candle", "Fire", "Screen", "Cycle"]}}
      },
      "u1": {"type": "integer"}, "u2": {"type": "integer"}
    }
  }
}
```

`light` replaces the name matching in `light_profile()`
(`src/views/screens/restaurant/RestaurantView.cpp`); the Classic entries get `light` values
equal to what the name matching produces today (HD-only, so no game-state effect).

Exit criterion: all four tables load through registries; golden fingerprints unchanged.

## Stage 2: mod assets (new pictures and sprite frames)

Problem: all pictures come from the game folder (`VgaLoader::load`, `BobLoader::load`); the
HD pack can only replace existing ones (`HdPack::vga`, `HdPack::bob_frame`); palettes come from
a C++ table (`AssetMetadataRegistry::metadata_`). The engine composes in 8-bit indexed colour
and lays HD texels on top (`Surface::attach_hd`).

Refactor:

1. A mod asset loader beside `VgaLoader`/`BobLoader`: keys with a mod prefix
   (`mod:<mod-id>/<path>`) read **indexed PNG** files (colour index = pixel value, palette
   declared in the asset's JSON or the furniture `sprite.palette`). `fileutils::find_file_path`
   must not resolve mod keys against the game folder.
2. A sprite-sheet abstraction so a furniture entry can name frames from a mod sheet instead
   of `bob/moebel.bob` frame numbers; `BobRenderer` and `RestaurantView::bob_renderer_for`
   choose the sheet per entry.
3. Palette metadata for mod pictures as data (`assets.json` in the mod), merged into the
   lookup used by `AssetMetadataRegistry::get_palette`.
4. HD art for mod pictures uses the existing pack rules, keyed by the mod key
   (`HdPack::normalize_key` gains the `mod:` prefix rule).
5. Tool: `tools/plmod/quantize.py` converts true-colour art to an indexed PNG for a chosen
   palette (palettes from the user's `gfx/palette.e`, read locally; same idea as
   `tools/hdpack/palettes.json`).

Risk: indexed art must fit the chosen palette; the room palette is shared by all furniture
(palette 4 for `bob/moebel.bob`). New furniture should use that palette.

## Stage 3: city abstraction

Problem: ten cities, fixed by `NUMBER_OF_CITIES` and fixed-size arrays; city files are found
by number; the map is 160x120 everywhere.

Refactor, in this order:

1. `CityDefinition` (new data struct) holding everything `CityFactory` now reads into members:
   map bytes and size, district grid, districts, actors, phone numbers, settings, food
   suppliers, building names, ingredient base prices, display name, world-map position.
   Two producers:
   * `ClassicCitySource` = today's `CityFactory::load_city_data`, `load_map_data`,
     `load_district_bez` (unchanged parsing);
   * `ModCitySource` reading the JSON below.
   `CityFactory::create_city` then builds `City` from a `CityDefinition`
   (`build_city_object`, `populate_properties` unchanged).
2. Replace per-city fixed arrays with containers keyed by `CityId`:
   `GameState::cities_`, `CityInitFlags` (`GameState.h`), `CityDayFlags`,
   `RestaurantDayFlags` (`TurnOrchestrator.h`), `Player::turnover_by_city_` (`Player.h`),
   `PlayerFinancials::weekly_city_profit_`, `days_present_` (`PlayerFinancials.h`),
   `WeaponMarket::slots_` (`WeaponMarket.h`), `opened_cities` (`MilestoneTracker.cpp`), the
   loop in `PlayerProfile.cpp`, `is_valid_city_id` (`GameState.h`), the console `city_id`
   range (`CommonArgs.h`), the random city in `MafiaConversation.cpp`. Save keys stay the
   same (arrays become arrays of the world's size).
3. City name: `City::name()` returns the definition's name; Classic keeps `core::city_names`.
4. Map size: make `MAP_TILES_PER_ROW/COLUMN` members of `City` and pass them where used:
   `City::index_to_x/y`, `x_y_to_index`, `district_index_by_xy_index`,
   `Property::calculate_demographics`, `RestaurantEconomy::calculate_demand_with_competition`
   (literal 159/119), `ControlMap` (20x15 cells, `CONTROL_MAP_CELLS`), `std::array<uint8_t,300>`
   in `City`/`CityFactory`, map drawing in `CityView`, `StatsRestaurants`, `MarketResearch`.
   The district grid keeps 8x8-tile cells; its size becomes `ceil(w/8) x ceil(h/8)`.
5. District index limit (16, from the bez nibble) applies only to Classic files; mod cities
   store a byte or more per cell.
6. Milestones tied to the city number (`ArrivalInParis + id`, `DominatesMarketIn*`,
   `GameState::on_city_entered`, `MilestoneTracker`) are emitted only for classic cities;
   Expanded cities use generic "arrival in a new city" entries with engine text (the
   original `t/leben.e` has no lines for them).

Proposed city schema (abridged):

```json
{
  "$id": "pizzatycoon/city.schema.json",
  "type": "object",
  "required": ["id", "name", "map", "districts", "actors", "food_suppliers", "prices", "world"],
  "properties": {
    "id": {"type": "string", "pattern": "^[a-z0-9_]+:[a-z0-9_]+$"},
    "name": {"type": "string"},
    "map": {
      "type": "object",
      "required": ["width", "height", "tiles", "district_cells"],
      "properties": {
        "width": {"type": "integer", "minimum": 40, "maximum": 400},
        "height": {"type": "integer", "minimum": 40, "maximum": 400},
        "tiles": {"type": "string", "description": "file in the mod: width*height bytes, same tile codes as mapN.dat"},
        "district_cells": {"type": "string", "description": "file: one byte per 8x8 cell = district index"}
      }
    },
    "districts": {
      "type": "array", "minItems": 1, "maxItems": 64,
      "items": {
        "type": "object",
        "required": ["name", "type", "average_size", "average_size_modifier", "price_level",
                     "rent_level", "premises_availability", "population", "average_income_factor",
                     "demographic_base", "demographic_factor", "service_speed_sensitivity",
                     "min_standard_pizzas", "min_user_pizzas"],
        "properties": {
          "name": {"type": "string", "maxLength": 23},
          "type": {"enum": ["residential", "industrial", "business", "leisure", "shopping", "traffic"]},
          "population": {"type": "array", "items": {"type": "number"}, "minItems": 6, "maxItems": 6},
          "traffic": {"type": "integer"}, "crime": {"type": "integer"},
          "alternate_control_color": {"type": "boolean"}
        }
      }
    },
    "actors": {
      "type": "object",
      "properties": {
        "realtors": {"type": "array", "items": {"$ref": "#/$defs/actor"}},
        "banks": {"type": "array"}, "insurance": {"type": "array"},
        "mayor": {"$ref": "#/$defs/actor"}, "police": {"type": "array"},
        "mafia": {"type": "array", "maxItems": 2}, "arms_dealers": {"type": "array"}
      }
    },
    "food_suppliers": {"type": "array", "items": {
      "type": "object",
      "required": ["category", "quality", "price_factor", "name", "min_contract", "loyalty", "head", "price_levels"],
      "properties": {"price_levels": {"type": "object", "additionalProperties": {"type": "integer", "minimum": 0, "maximum": 255}}}
    }},
    "prices": {"type": "object", "description": "ingredient id -> base price in cents"},
    "building_names": {"type": "object", "description": "church/bank/cinema/school/disco/bar/department_store/hotel -> list"},
    "world": {"type": "object", "required": ["x", "y"], "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}, "region": {"type": "string"}}}
  },
  "$defs": {"actor": {"type": "object", "required": ["name", "head"],
            "properties": {"name": {"type": "string"}, "head": {"type": "integer"}, "phone": {"type": "integer"},
                           "settings": {"type": "array", "items": {"type": "integer"}, "maxItems": 4}}}}
}
```

Field meanings map one-to-one to the binary layout in
[file-formats 1.8](file-formats.md#18-city-data-datcitynpc), so a local converter
(`tools/plmod/city2json.py`, reading the user's `dat/` files and writing to a local folder,
never to the repository) can dump Classic cities as reference material for authors.

Exit criterion: Classic cities load through `ClassicCitySource`; golden fingerprints
unchanged; a test city from JSON loads in a debug build.

## Stage 4: Expanded mode

1. **Mode flag.** Add `GameMode::EXPANDED` (`src/core/GameTypes.h`) or, cleaner, a
   `world` field in `init::Config` (`src/core/init/NewGameConfig.h`) with value `classic` or a
   world id; stored in the save (`state_to_json`). Classic remains the default and keeps
   every current code path.
2. **World definition** (`world.json` in a mod):

   ```json
   {
     "id": "mymod:europe_plus",
     "cities": ["classic:paris", "classic:berlin", "mymod:amsterdam"],
     "start_cities": ["classic:paris", "mymod:amsterdam"],
     "map_picture": "mymod:gfx/worldmap",
     "travel": {
       "cost_formula": "manhattan",
       "dollars_per_unit": 1,
       "difficulty_factor": true,
       "routes": [
         {"from": "classic:paris", "to": "mymod:amsterdam", "cost": 450, "blocks": 6}
       ]
     },
     "win_condition": {"type": "market_share", "percent": 5, "cities": "any"}
   }
   ```

   Today's cost is `Manhattan distance between city_coords x (difficulty + 1)` dollars and
   travel costs no time (`EuroMap::calculate_cost`, `EuroMap::attempt_flight` sets
   `pending_screen_charge(0)`). Expanded mode can keep that formula or use explicit routes,
   and may charge time blocks (`GameState::advance_time_blocks`).
3. **World map screen.** `EuroMap` uses a fixed picture (`gfx/euromap`) and `city_coords`.
   For Expanded, either a new `WorldMap` view (same interaction code, positions from the world
   definition, picture from the mod) or `EuroMap` with injected coordinates and picture.
   Classic keeps `EuroMap` as is.
4. **Win condition.** Generalise `MilestoneTracker::check_world_domination` for Expanded
   (`cities` = any / all of the world / a list); Classic keeps `WinCondition`.
5. **Dormant cities.** Today cities without a human are not ticked
   (`TurnOrchestrator::tick_active_cities`) and away cities pay an extrapolated weekly profit
   (`collect_weekly_city_profits`). Keep this model in Expanded; it scales to many cities.

## Stage 5: other content types

| Content | Change | Code |
|---|---|---|
| Ingredients | allow ids beyond the 4x24 Classic blocks (registry index), per-category product counts from data, supplier price levels as a map; Classic keeps `max_ingredient_count = 96` arrays for its own files | `IngredientType.h`, `IngredientArray.h`, `FoodSupplier.h` (`VEGETABLE_COUNT`.., `price_levels_[22]`), `City::ingredient_base_prices`, `PizzaDemand::find_primary_ingredients`, `Pizza::calculate_herb_score` (herb flag as data) |
| Standard recipes | a recipe JSON equivalent to the recipe record (file-formats 1.12) plus a "standard" flag and cook book stages; Classic keeps the 14 | `StandardPizza.h`, `PlayerRecipes::standard`, `Menu::count_standard`, `PizzaFactory` |
| Competition references | list of reference recipes per world | `CompetitionCoordinator::reference_pizza_index` |
| AI restaurants | layout files listed in data, selection weights by size and consumer bias | `ai_layout_index`, `ai_demographic_bias`, `RestaurantLayoutFactory::ai_layout_count` |
| Newspaper events | event JSON with an effect list built from the primitives that exist: `trend` (`IngredientTrendSystem::apply_effect` with linear/exponential/countdown), `embargo` (category, weeks), `spoilage` (rate, weeks, seasons), `interest` (min/max delta), `taste` (consumer type, values), `follow_up` (event, days); text and picture from the mod | `NewspaperEventSystem::execute_named_event`, `ArticleDatabase` |
| Commercials and posters | list in data (frames, play type, title text) | `CommercialFactory::create`, `NUMBER_OF_POSTERS`, `NUMBER_OF_TV_ADS` |
| Trainings | free number per job, `level` field | `TrainingFactory`, `Training` |
| Characters | portraits as indexed PNG + JSON stats | `CharacterFactory`, `JobMarket::ORIGINAL_SLOTS` |
| Milestones | new ids with engine-provided text and score | `MilestoneId.h`, `milestone_data`, `CareerLog` |

Proposed newspaper event schema (abridged):

```json
{
  "type": "object",
  "required": ["id", "weight", "article", "effects"],
  "properties": {
    "id": {"type": "string"},
    "weight": {"type": "integer", "minimum": 0},
    "article": {"type": "object", "required": ["title", "body"],
                "properties": {"title": {"type": "string"}, "body": {"type": "string"}, "picture": {"type": "string"}}},
    "effects": {"type": "array", "items": {"oneOf": [
      {"type": "object", "required": ["trend", "ingredient", "mode", "initial", "control"],
       "properties": {"mode": {"enum": ["linear", "exponential", "countdown"]}}},
      {"type": "object", "required": ["embargo", "category", "weeks"]},
      {"type": "object", "required": ["spoilage", "rate", "weeks"]},
      {"type": "object", "required": ["interest", "min", "max"]},
      {"type": "object", "required": ["taste", "consumer", "values"]},
      {"type": "object", "required": ["follow_up", "event", "days_min", "days_max"]}
    ]}}
  }
}
```

## Stage 6: generated content (Expanded only)

* **City generator** (tool first, engine later): produce a `CityDefinition` from a seed using
  the tile codes of file-formats 1.9 (roads with the `RoadType` variants, premises with low
  nibble `0xA`, named buildings `0x88..0xF8`, depots `0x04`), districts with parameters drawn
  from the ranges seen in the Classic cities (read locally by the converter), suppliers and
  actors from name lists in the mod. Validate with the same rules the engine uses
  (`City::is_business_premise_by_type`, property size limits 10..20,
  `CityFactory::populate_properties`).
* **Determinism**: generation must use its own RNG (seeded from the world seed), never the
  simulation stream (`engine::random`), and the generated definition is saved in or next to
  the save so a reload does not depend on the generator version.
* Recipes, events and names can be generated the same way; all output is ordinary mod data
  that the validator checks.

## Stage 7: SDK packaging

* Mod layout:

  ```
  mods/<mod-id>/
    mod.json            id, name, version, engine version range, depends, modes ("expanded")
    content/            furniture.json, ingredients.json, ... (registry additions)
    cities/<id>.json    + tiles/<id>.bin, cells/<id>.bin
    worlds/<id>.json
    gfx/, bob/          indexed PNG art (+ assets.json for palettes)
    hd/                 optional HD pack layer for the mod's own keys
    text/<lang>.json    strings for new content
  ```

* Tools in `tools/plmod/` (Python, run with `python3 -I`, mirroring `tools/hdpack/`):
  `validate`, `quantize`, `city2json` (local only), `gen-city`, `pack` (zip with hashes).
* Engine: a mod list in `settings.conf`, applied only to new Expanded games; the save records
  the mod ids and hashes and refuses to load when they are missing.
* Documentation: this blueprint plus generated schema docs.

## Risks

| Risk | Where | Mitigation |
|---|---|---|
| RNG order changes break Classic | shared simulation stream (`src/engine/Random.cpp`) | golden fingerprints (stage 0); no new simulation draws in Classic paths; generators use their own streams |
| Save compatibility | `SaveGameManager::migrate_save`, numeric ids in saves, cities rebuilt from `dat/` on load | version bump with migration; `content` block; store mod city definitions or their hashes |
| Fixed-size arrays and indices | `NUMBER_OF_CITIES`, map constants, `std::array<...,300>`, 96-ingredient arrays, 22 price levels, 6 consumer types everywhere | change one subsystem per stage, behind tests |
| Text files indexed by line number | `t/*.e`, `t/leben.e`, `t/levent.e`, `z/artikel*.e` | new content brings its own text table; never edit original files |
| Indexed colour | 8-bit composition, palettes per picture (`AssetMetadata.cpp`) | indexed PNG with declared palette; quantizer tool; HD overlay for detail |
| Audio slot order | `SoundId`/`MusicId` assigned by load order (`AudioManager::load_default_assets`) | mod audio as named entries, separate from the classic slots |
| UI layout assumes Classic counts | Euro map positions, `PlayerProfile` city loop, 9 posters/ads, 24-item menus, 6 staff slots | Expanded-specific views where the layout cannot stretch |
| Legal | original assets and text | mods never contain them; converters write locally; the validator flags files whose hashes match exported originals |
| Two code paths drift | Classic vs Expanded | Expanded reuses Classic systems with data injected; differences are explicit in `world`/rules data, not `if` chains |

## Ten blockers, in the order to remove them

1. Enum-keyed content tables: `data_file::load` requires exact enum key sets
   (`src/core/factories/DataFile.h`; `FurnitureType`, `IngredientType`, `SupplierID`,
   `TrainingType`).
2. `NUMBER_OF_CITIES = 10` and the fixed per-city arrays (`src/core/util/Constants.h`,
   `GameState::cities_`, `TurnOrchestrator::CityDayFlags`, `Player::turnover_by_city_`,
   `PlayerFinancials`, `WeaponMarket::slots_`).
3. Cities exist only as original binary files found by number
   (`CityFactory::load_city_data`, `load_map_data`, `load_district_bez`), and saves rebuild
   them from those files (`GameState::ensure_city_loaded` in `SaveGameManager::json_to_state`).
4. Map and grid sizes are compile-time constants (160x120, 20x15, 300-byte grid) with literals
   `159`/`119` in `RestaurantEconomy::calculate_demand_with_competition`.
5. City presentation is hard-coded: names (`core::city_names`), Euro map picture and positions
   (`EuroMap.cpp` `city_coords`), flight cost (`EuroMap::calculate_cost`).
6. City-numbered milestones and win conditions (`MilestoneId` 100-119,
   `GameState::on_city_entered`, `MilestoneTracker::check_world_domination`).
7. New art cannot be added: loaders read only the game folder, the HD pack only replaces
   existing pictures/frames (`HdPack::vga`, `HdPack::bob_frame`), palettes are a C++ table
   (`AssetMetadataRegistry::metadata_`).
8. Ingredient id space and supplier product arrays are fixed (`IngredientType.h` ranges,
   `max_ingredient_count = 96`, `FoodSupplier::price_levels_` of 22, category counts in
   `FoodSupplier.h`).
9. Standard pizzas, competition references and AI layouts are numbered files with fixed
   counts (`StandardPizza`, `first_reference_pizza = 20`, `ai_layout_count = 24`,
   `ai_layout_index`).
10. Events, commercials and posters are code (`NewspaperEventSystem::execute_named_event`,
    `CommercialFactory::create`, `NUMBER_OF_POSTERS`/`NUMBER_OF_TV_ADS`), and their texts are
    line-indexed original files.
