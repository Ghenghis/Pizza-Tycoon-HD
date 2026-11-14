# Content data

What game content is data, where it lives, what every field means, and what is still
hard-coded in C++. A mod that adds cities, maps, furniture, recipes or events has to deal
with every table in this file.

Sources of content:

| Source | Editable without rebuilding? | Notes |
|---|---|---|
| `data/*.json` (shipped with the program) | values only | keys are C++ enum names; adding or removing an entry needs a code change (`data_file::load`, `src/core/factories/DataFile.h`) |
| Original game files (`dat/`, `chr/`, `r/`, `f/`, `q/`, `d/`, `t/`, `z/`, `gfx/`, `bob/`, `sound/`) | yes, in their binary formats ([file-formats.md](file-formats.md)) | the engine reads them from the user's copy of the game; they cannot be redistributed |
| Tables in C++ source | no | listed in section 3 |

---

## 1. `data/*.json`

All four files are JSON objects keyed by enum value names. `data_file::load<T, E>()` requires
the key set to be exactly the set of values of enum `E`: unknown keys (including `_comment`)
and missing values are fatal. Arrays have fixed lengths (`data_file::array_field<T, N>`);
enum-valued fields must use the exact enumerator name (`data_file::enum_field`).

### 1.1 `furniture.json`

Keys: `FurnitureType` (`src/core/restaurant/RestaurantTypes.h`), values 0..162; the value is
the furniture id stored in layout files (`f/fN.pc`, `q/fN.pc`) and save games. Reader:
`read_furniture()` in `src/core/factories/FurnitureFactory.cpp`. Struct: `FurnitureData`
(`src/core/restaurant/Furniture.h`).

| Field | Type | Meaning | Used in |
|---|---|---|---|
| `name` | string | display name; also matched by substring for HD light sources ("Candle", "Electric", "Jukebox", "Telephone") | purchase lists, `light_profile()` in `src/views/screens/restaurant/RestaurantView.cpp` |
| `category` | `FurnitureCategory`: `OTHER`, `TABLE`, `CHAIR`, `DECOR`, `ENTERTAINMENT`, `GAMES`, `SLOT_MACHINES`, `UNKNOWN`, `KITCHENWARE`, `UNKNOWN3` | behaviour class. `KITCHENWARE` goes in the kitchen and is drawn from `bob/kuebobs.bob`; `GAMES` (x3) and `SLOT_MACHINES` (x2) earn coin income from young people (`Restaurant::calculate_slot_machine_income`); `CHAIR` linked to a table is a seat | `Restaurant.cpp`, `RestaurantView::bob_renderer_for`, `GuestWalkScene` |
| `supplier` | int | index into `SupplierID` (who sells it) | `FurnitureFactory::all_furniture_by_supplier` |
| `price` | int, dollars | purchase price | `Restaurant::purchase_furniture`, layout value |
| `quality` | int 0..99 | for ovens: averaged into oven quality, weighted by `100 / bake_time` (`Restaurant::calculate_average_oven_quality`); also summed in furniture statistics | pizza quality rating |
| `capacity` | int | tables: seats ("room for N people"); ovens: pizzas per bake | `calculate_daily_oven_throughput`, seat counts |
| `bake_time` | int, minutes | ovens only (non-zero for the 13 kitchen items); daily oven throughput = capacity x operating minutes / bake_time | `Restaurant::calculate_daily_oven_throughput` |
| `bobs` | 4 ints | sprite frame for each rotation 0..3 in `bob/moebel.bob` or (KITCHENWARE) `bob/kuebobs.bob` | `BobRenderer`, `PlacedFurniture` |
| `placement_layer` | `Seating`, `Tables`, `Top` | draw/placement layer: chairs, floor items, items placed on top of tables | `FurniturePlacementController`, `RestaurantView::render` |
| `popularity` | 6 ints (consumer types) | appeal per consumer type, summed over placed furniture plus floor | `RestaurantEconomy::calculate_furniture_popularity` |
| `role` | optional `FurnitureRole`: `None`, `AshesSmall`, `AshesLarge`, `AshesKitchen`, `Jukebox`, `Tv` | special rules: what remains after an attack, which popup a click opens | `FurnitureFactory::furniture_with_role` |
| `u1` | int (2, 3 or 4) | unknown; read, no use found | |
| `u2` | int | unknown; read, no use found | |

Consumer type order for every 6-value array in this file and the others
(`enum class ConsumerType`, `src/core/Demographics.h`): children, young people, blue collar,
white collar, business people, senior citizens.

### 1.2 `ingredients.json`

Keys: `core::food::IngredientType` (`src/core/food/IngredientType.h`). The numeric value is
the ingredient id used in recipe files, city price tables and supplier price levels. Ids are
grouped in fixed 24-slot blocks: vegetables 0-21, meat 24-40, fruit 48-60, cheese and herbs
72-84; `max_ingredient_count = 96`. Reader: `read_ingredient()` in
`src/core/factories/IngredientFactory.cpp`; struct `IngredientData` (`src/core/food/Ingredient.h`).

| Field | Type | Meaning | Used in |
|---|---|---|---|
| `category` | `VEGETABLES`, `MEAT`, `FRUIT`, `CHEESE` | which supplier type sells it | `CategoryRegistry` |
| `weight_large` / `weight_medium` / `weight_small` | int, grams | weight of one placed piece when whole / chopped / minced | `Topping::weight()` |
| `calories` | int per 100 g | calories of the pizza | `Pizza::calculate_calories` |
| `intensity` | int | taste weight: flavour uses `intensity >> 1`; primary-ingredient score = grams x intensity | `Pizza::calculate_flavor_percentages`, `PizzaDemand::find_primary_ingredients` |
| `fat`, `sweet`, `salty`, `hot` | int | per-ingredient taste values; pizza taste = clamp(sum(value x grams x (intensity>>1)) / (250 + total grams), 0, 10) | `Pizza::calculate_flavor_percentages` |
| `bobs` | 12 ints | frame in `bob/zutaten.bob` for each topping state: 0-3 whole (rotations), 4-7 chopped, 8-11 minced | `Topping::bobs()`, `core::food::topping_size_fn` |

Not in the file: the display name (`to_string(IngredientType)` in `IngredientType.h`), the
herb rule (cheese-category ids >= `PARSLEY` count as herbs, `Pizza::calculate_herb_score`),
and per-city base prices (`dat/cityN.pc`, offset 2026).

### 1.3 `suppliers.json`

Keys: `SupplierID` (`src/core/supply/Supplier.h`), the furniture, flooring, kitchen and extras
vendors. Food suppliers are not here: they come from each city file. Reader:
`read_supplier()` in `src/core/factories/SupplierFactory.cpp`.

| Field | Type | Meaning | Used in |
|---|---|---|---|
| `type` | `SupplierType`: `FLOORING`, `FURNITURE`, `KITCHENWARE`, `EXTRAS` (`VEGETABLES`..`CHEESE` exist in the enum but are not used in the file) | purchasing screen tab | `SupplierFactory::all_suppliers_by_type` |
| `name` | string | display name | |
| `quality` | u8 | vendor quality; the purchasing list is sorted by `quality >> 4`, then id; floor patterns take it | `SupplierFactory::all_suppliers`, `FloorPatternFactory` |
| `price_per_tile` | u16, dollars | flooring price per tile | `FloorPattern::price_per_tile` |
| `customer_ratings` | 6 x i8 | floor appeal per consumer type, multiplied by the pattern factor / 2 | `FloorPattern::popularity_for` |
| `introduction_year`, `withdrawal_year` | u16 | the vendor is listed while `introduction_year <= year <= withdrawal_year`; 2200 means never withdrawn | `all_suppliers_by_type` |

Floor tile art for vendor id `v` is `gfx/k{v+1}.vga`; the available patterns per vendor are in
`d/kachinfo.pc`.

### 1.4 `trainings.json`

Keys: `TrainingType` (`src/core/character/Training.h`), 6 courses per job in enum order (the
position in its job is the course level). Reader: `read_training()` in
`src/core/factories/TrainingFactory.cpp`.

| Field | Type | Meaning |
|---|---|---|
| `name` | string | display name |
| `job` | `CHEF`, `WAITER`, `MANAGER` (`core::character::JobRole`) | who can take it |
| `duration` | int, days | time away |
| `min_level` | int | required skill score |
| `cost` | int, dollars | fee |
| `score_increase` | int | skill gained on success |
| `pizzas_day_increase` | int | daily output gained |

Completing the last course of a job fires a mastery milestone (`master_milestone` lambda in
`Restaurant::end_of_day`).

### 1.5 `fonts/hd_fonts.json`

See [file-formats.md 2.2](file-formats.md#22-datafontshd_fontsjson). Not an `enum`-checked
table: names are font file names and unknown keys are ignored.

---

## 2. Original data files that act as content

Cities, actors, food suppliers, standard recipes, prebuilt restaurants and characters are read
from the user's game copy. Each is identified by a number, and the number range is fixed in
code:

| Content | Files | Count fixed by |
|---|---|---|
| Cities | `dat/cityN.pc`, `dat/mapN.dat`, `dat/bezN.dat` | `NUMBER_OF_CITIES = 10` (`src/core/util/Constants.h`); file names built from the id in `CityFactory` |
| Districts per city | district records in `cityN.pc` | district index is the low nibble of `bezN.dat`, so at most 16 |
| Actors per city | `cityN.pc` | 5 types x 10 (`NUMBER_OF_ACTOR_TYPES`, `MAX_ACTOR_PER_TYPE`, `src/core/factories/CityFactory.h`) |
| Food suppliers per city | `cityN.pc` | count byte at offset 9; 22 products per supplier |
| Named buildings | `cityN.pc` | 8 types x 24 names |
| Characters | `chr/cNe.pc`, `chr/amarkt`, `t/double.e` | `JobMarket::ORIGINAL_SLOTS = 100` plus one double per line pair |
| Standard recipes | `r/r0.pc`..`r/r13.pc` | `StandardPizza` enum (14) |
| Competition references | `r/r20.pc`..`r/r69.pc` | `first_reference_pizza = 20`, `max_competition_count = 40`, `+ rand(10)` (`CompetitionCoordinator`) |
| AI restaurants | `f/f1.pc`..`f/f24.pc` | `RestaurantLayoutFactory::ai_layout_count = 24`; index formula in `ai_layout_index()` (`src/core/state/RealEstate.cpp`) |
| Quick-start restaurants | `q/f1.pc`..`q/f8.pc` | `GameState::finalize_initialization` |
| Floor patterns | `d/kachinfo.pc` | count byte |
| Missions | `m/*.mis` + user missions | any number (`list_available_missions`) |
| Newspaper articles | `z/artikel0.e`, `z/artikel1.e`, `t/artikel3.e` | 50 + 50 + 8 |

---

## 3. Hard-coded content tables in C++

These would have to become data for a modding SDK. "Classic-critical" marks tables whose
exact values define the 1994 rules and must stay identical in Classic mode.

### 3.1 World and cities

| Table | Location | Content |
|---|---|---|
| City count | `NUMBER_OF_CITIES = 10`, `src/core/util/Constants.h` | sizes of `GameState::cities_`, `CityInitFlags`, `TurnOrchestrator::CityDayFlags`/`RestaurantDayFlags`, `Player::turnover_by_city_`, `PlayerFinancials::weekly_city_profit_`/`days_present_`, `WeaponMarket::slots_`, console `city_id` range |
| City names | `core::city_names`, `src/core/city/CityNames.h` | 10 English names; `City::name()` |
| Euro map positions | `city_coords[]`, `src/views/screens/EuroMap.cpp` | click positions; flight and export cost = Manhattan distance x (difficulty + 1) dollars (`EuroMap::calculate_cost`) |
| Euro map picture | `gfx/euromap` | one picture, positions baked in |
| City milestones | `MilestoneId::ArrivalInParis..ArrivalInLondon` (100-109), `DominatesMarketInParis..London` (110-119), `CentralWarehouseIn5Cities`/`In10Cities`, `OutletsInEveryWesternCity` (`src/core/events/MilestoneId.h`); `GameState::on_city_entered` adds the city id to 100 | per-city CV lines |
| Win condition | `MilestoneTracker::check_world_domination` (`src/core/state/MilestoneTracker.cpp`) | `Paris` checks `CityId{0}`; `AllCities` needs all `NUMBER_OF_CITIES` initialised; threshold `win_threshold = 5` percent of control-map cells (Classic-critical) |
| Map size | `MAP_TILES_PER_ROW = 160`, `MAP_TILES_PER_COLUMN = 120`; literal `159`/`119` in `RestaurantEconomy::calculate_demand_with_competition` | |
| District grid | `NUMBER_OF_DISTRICT_CELL_COLUMNS = 20`, `..._ROWS = 15`; `std::array<uint8_t, 300>` in `City`/`CityFactory`; `ControlMap` 20x15 cells (`src/core/city/ControlMap.h`) | |
| District colours | `CityFactory::load_color_definition` | 12 palette indices |
| Tile semantics | `CityFactory::populate_types`, `City::is_business_premise_by_type`, `is_named_building`, `is_road_tile`, `tile_description`, `building_type` | tile byte meaning (file-formats 1.9) |
| Tile population weights | `residential_pop_table[8][6]`, `Property::calculate_tile_population` (`src/core/city/Property.cpp`) | Classic-critical |
| Building type names | `City::building_type_name` | 8 names |
| Premise generation | `CityFactory::populate_properties`: `min_premise_side = 10`, `max_premise_side = 20`, `min_kitchen_width = 12`; price = price level x w x h; `depot_price_multiplier = 5` (`Property.h`) | Classic-critical |
| Actor roles by position | `CityFactory::build_city_object` | authorities entry 0 = mayor, rest police; others entries 0-1 = mafia, rest arms dealers |
| Actor backgrounds | `Actor::Actor` sets `background_id = ActorType` value, drawn as `gfx/back{N}` (`ActorView.inl`) | `ActorType` values 10-22 (`src/core/character/Actor.h`) |
| Starting interest rate | `starting_interest_rate = 8`, `src/core/city/City.h` | |
| Advertising rates | `City::init_advertising_options` | random poster and TV rates |
| Supplier stock | `City::init_supplier_stock_levels`, `replenish_supplier_stock_levels` | 8-13 t initial, refill rules |
| Map fix | `CityFactory::load_map_data` (`stray_front_index = 11589` in city 1) | |

### 3.2 Food

| Table | Location | Content |
|---|---|---|
| Ingredient ids and names | `enum class IngredientType`, `to_string()`, `ingredient_ranges`, `src/core/food/IngredientType.h` | ids, names, category ranges |
| Products per supplier category | `VEGETABLE_COUNT = 22`, `MEAT_COUNT = 17`, `FRUIT_COUNT = 13`, `CHEESE_COUNT = 13` (`src/core/supply/FoodSupplier.h`); category metadata `CategoryRegistry::metadata_` (`src/core/food/IngredientCategory.cpp`) | |
| Standard pizzas | `enum class StandardPizza`, `standard_pizza_names`, `cook_book_stages` (`src/core/food/StandardPizza.h`) | 14 pizzas, names, cook book page stages |
| Pizza scoring constants | `src/core/food/Pizza.cpp` (top of file): pizza centre, appearance threshold, dough weight 250 g, normality thresholds, judge constants | Classic-critical |
| Taste profiles | `default_customer_taste_profiles()`, `src/core/food/CustomerTasteProfiles.h` | per consumer type |
| Menu limits | `Menu::MAXIMUM_PIZZAS = 24`, `min_price = 1`, `max_price = 50` dollars (`src/core/restaurant/Menu.h`) | |
| Difficulty price factors | `FoodSupplier::price_multiplier`: 0.6, 0.8, 1.0, 1.5, 1.9 | Classic-critical |
| Supplier price dynamics | `FoodSupplier::daily_update` constants | Classic-critical |
| Initial pizzas | `number_of_standard_pizzas_to_load = 4`, `GameState::load_initial_pizzas` | |

### 3.3 Restaurants

| Table | Location |
|---|---|
| Light sources by furniture name/category | `light_profile()`, `src/views/screens/restaurant/RestaurantView.cpp` (HD only) |
| Day/night ambient light | `RestaurantView::lighting` |
| Staff limits | `MAXIMUM_STAFF_PER_CATEGORY = 6`, `max_ingredient_stock_kg = 650` (`src/core/restaurant/Restaurant.h`) |
| Opening hours | `Restaurant::DEFAULT_OPENING_TIME` 9:00, `DEFAULT_CLOSING_TIME` 21:00 |
| Floor sprite rules | `FLOOR_TILE_OFFSET = 2`, tile size 24x22 (`RestaurantView.h`, `LegacyConstants.h`) |
| Slot machine income multipliers | `Restaurant::calculate_slot_machine_income` |
| Electricity | `Restaurant::calculate_electricity_cost`: oven throughput x 3 dollars |
| AI layout choice | `ai_layout_index`, `ai_demographic_bias` (`src/core/state/RealEstate.cpp`); `ai::opening_furniture_count = 20` (`src/core/restaurant/AIEconomy.h`) |

### 3.4 People and careers

| Table | Location |
|---|---|
| Milestones | `enum class MilestoneId` (1..140), `milestone_data` (full-screen background and score per id, `src/core/events/CareerLog.cpp`) |
| Social/underworld ranks | `social_score_table`, `scaled_threshold` (`src/core/character/SocialStatus.h`) |
| Awards | `AwardCategory`, `AwardLevel` (`src/core/character/Awards.h`); mayor awards `MayorAwardType` (`src/core/city/MayorOffice.h`) |
| Company names | `init::random_company_names()` (`src/core/init/CompanyNames.cpp`) |
| Players | `NUMBER_OF_PLAYERS = 8` |
| Difficulty levels | `init::DifficultyLevel` 1..5 (`src/core/init/NewGameConfig.h`) |
| Job market thresholds | `JobMarket::end_of_week` |
| Staff events | `StaffEventData::Type` (`src/core/restaurant/StaffEventData.h`), probabilities in `StaffEvents.cpp` |
| Tutorial | `src/core/tutorial/TutorialSteps.cpp`, `TutorialGame.cpp` (city 6, seed 292, character 0) |

### 3.5 Events, media, crime

| Table | Location |
|---|---|
| Newspaper events | `enum class NewspaperEvent` (50) and effects in `NewspaperEventSystem::execute_named_event` (`src/core/newspaper/NewspaperEventSystem.cpp`) |
| Article pictures | `flavor_image_map_`, `event_image_map_` (`src/core/newspaper/ArticleDatabase.h`) |
| Article id encoding | `NewspaperState` (`src/core/newspaper/NewspaperState.h`) |
| TV commercials | `CommercialFactory::create` switch (`src/core/factories/CommercialFactory.cpp`): file, frames, play type, title text |
| Poster/TV counts | `NUMBER_OF_POSTERS = 9`, `NUMBER_OF_TV_ADS = 9` (`src/core/marketing/AdvertisingCampaign.h`) |
| Weapons | `enum class core::item::Item` (8) (`src/core/item/Item.h`); base prices and price walk in `src/core/economy/WeaponMarket.cpp` |
| Insurance | `InsuranceType` (`src/core/character/InsurancePlayerState.h`) |
| Mafia missions | `MafiaMissionType` (`src/core/character/MafiaPlayerState.h`), logic in `MafiaConversation.cpp` |
| Bank | `MAX_NUMBER_OF_LOANS = 4`, investment constants (`BankConversation.cpp`) |

### 3.6 Presentation

| Table | Location |
|---|---|
| Palette per picture | `AssetMetadataRegistry::metadata_` (`src/resources/AssetMetadata.cpp`) |
| Icon atlases | `Blitmap1Icon..Blitmap4Icon` (`src/resources/BlitmapIcons.h`), `Icon` (`src/views/ui/Icon.h`) |
| Sound and music slots | `SoundId`, `MusicId` (`src/engine/audio/AudioManager.h`), load order in `AudioManager::load_default_assets` |
| Fonts | `FontFace`, `FontManager::font_map_` |

---

## 4. What blocks new content today

| Goal | Blocking code |
|---|---|
| New furniture | `FurnitureType` enum and `data_file::load` exact key set; sprite frames must already exist in `bob/moebel.bob` / `bob/kuebobs.bob` (the HD pack can only replace existing frames, `HdPack::bob_frame`); light profiles keyed by name |
| New ingredient | `IngredientType` ids inside fixed category blocks; names in `to_string`; per-category product counts; 22-entry supplier price arrays; 96-entry city price table |
| New standard recipe | `StandardPizza` enum, names, cook book stages, `r/rN.pc` naming; "standard" means slot < 14 (`Menu::count_standard`, `STANDARD_PIZZA_COUNT`) |
| New furniture/floor vendor | `SupplierID` enum; floor art `gfx/k{id+1}`; `kachinfo.pc` |
| New training | `TrainingType` enum; 6 courses per job assumed by level position |
| New city | `NUMBER_OF_CITIES`; `city_names`; `city_coords`; city milestones; file naming from id; per-city fixed arrays; save stores only the id |
| New or bigger map | 160x120 and 20x15 constants, 300-byte grid, literal 159/119; 16-district limit |
| New district data | binary record in `cityN.pc` |
| New event | `NewspaperEvent` enum and switch; article text in `z/artikel1.e` with ids 51-100 |
| New commercial or poster | `CommercialFactory` switch; 9-slot arrays |
| New picture | palette table entry; VGA/BOB loaders read only from the game folder |
