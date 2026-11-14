# Debug console and `--exec` command reference

This file was generated from the `CommandDef` and `ArgSpec` initialisers in the source by a
read-only extraction script (kept outside the repository). It lists every command registered
in `register_debug_commands()` (`src/core/debug/DebugCommands.cpp`) and the screen commands
added by `engine::make_screen_commands()` (`src/engine/debug/ScreenCommands.cpp`, registered in
`Game::run` at `src/engine/Game.cpp`).

## How commands are run

| Way | How | Source |
|---|---|---|
| In-game console | Toggle with the `ToggleConsole` action. It has no default key (`declare_unbound(Action::ToggleConsole)`); bind one on the Key bindings screen or in `bindings.conf`. | `src/engine/input/DefaultBindings.cpp`, `src/views/layers/ConsoleLayer.cpp`, `src/core/debug/DebugConsole.cpp` |
| One line at start-up | `"Pizza Legacy" console <words...>`: the rest of the command line is one console line, run after start-up. | `parse_args()` in `src/Main.cpp`; `Game::run` in `src/engine/Game.cpp` |
| Script, then exit | `--exec <file>`: one command per line; leading/trailing blanks are stripped; empty lines and lines starting with `#` are skipped; each command's output lines are printed to stdout; a failed command prints `error: command failed: <line>` to stderr and the run continues. The window is created hidden (`SDL_WINDOW_HIDDEN`). | `Game::run_exec_script()` in `src/engine/Game.cpp` |

## Syntax

* Commands are trees: `restaurant stock buy 3 0 10`. The first word is looked up by name or alias
  (`CommandRegistry::find_command`, `src/core/debug/command/CommandRegistry.cpp`); sub-commands are
  resolved by `CommandParser::parse` (`src/core/debug/command/CommandParser.cpp`).
* Tokens are split on blanks; single and double quotes group words (`CommandTokenizer`,
  `src/core/debug/command/CommandTokenizer.cpp`).
* `@` stands for the default of an argument that has a default provider (active player,
  active restaurant, active city, current date); `@` on any other argument is an error
  (`CommandParser.cpp`).
* A node can be both a namespace and a leaf (for example `game date` prints the date and
  `game date set` changes it).

Argument types (`src/core/debug/command/ArgType.h`, parsed in `src/core/debug/command/TypeParser.cpp`):

| Type | Accepted input |
|---|---|
| String | any token |
| Int | 64-bit integer (`std::stoll`) |
| Money | whole dollars, converted to cents (`parse_money`) |
| PlayerId / RestaurantId / CityId | non-negative integer, or `@` for the active one |
| Bool | `true`/`false`, `yes`/`no`, `1`/`0` |
| ItemId | inventory item id (`core::item::Item`, `src/core/item/Item.h`: 0 SmellyCheese .. 7 Bomb) |
| RecipeId | recipe id |
| Date | a non-negative day offset from the game start date 1-Mar-1995 (`parse_date`) |

Common trailing arguments come from `src/core/debug/command/CommonArgs.h`:
`player_id` (0..7, default active player), `restaurant_id` (default active restaurant),
`city_id` (0..9, default active city).

## Command trees

| Root | Aliases | Description | Built by |
|---|---|---|---|
| `player` | `p` | Player management commands | `make_player_commands()` in `src/core/debug/commands/PlayerCommands.cpp` |
| `restaurant` | `r`, `rest` | Restaurant management commands | `make_restaurant_commands()` in `src/core/debug/commands/RestaurantCommands.cpp` |
| `depot` | `d`, `dep` | Depot management commands | `make_depot_commands()` in `src/core/debug/commands/DepotCommands.cpp` |
| `game` |  | Game state management | `make_game_commands()` in `src/core/debug/commands/GameCommands.cpp` |
| `nav` | `ns` | Navigation commands | `make_nav_commands()` in `src/core/debug/commands/NavCommands.cpp` |
| `debug` |  | Debug and diagnostic commands | `make_debug_commands()` in `src/core/debug/commands/DebugCommands.cpp` |
| `news` |  | Newspaper and trend system | `make_newspaper_commands()` in `src/core/debug/commands/NewspaperCommands.cpp` |
| `city` |  | City-level commands | `make_city_commands()` in `src/core/debug/commands/CityCommands.cpp` |
| `mafia` |  | Mafia interaction commands | `make_mafia_commands()` in `src/core/debug/commands/ActorCommands.cpp` |
| `police` |  | Police interaction commands | `make_police_commands()` in `src/core/debug/commands/ActorCommands.cpp` |
| `bank` |  | Bank interaction commands | `make_bank_commands()` in `src/core/debug/commands/ActorCommands.cpp` |
| `mayor` |  | Mayor interaction commands | `make_mayor_commands()` in `src/core/debug/commands/MayorCommands.cpp` |
| `staff` | `st` | Staff management and event testing commands | `make_staff_commands()` in `src/core/debug/commands/StaffCommands.cpp` |
| `armsdealer` |  | Arms dealer commands | `make_armsdealer_commands()` in `src/core/debug/commands/ActorCommands.cpp` |
| `tutorial` |  | Tutorial commands | `make_tutorial_commands()` in `src/core/debug/commands/TutorialCommands.cpp` |
| `dump` |  | Dump debugging information (alias for 'debug dump') | `make_dump_command()` in `src/core/debug/commands/DebugCommands.cpp` |
| `help` |  | Show help for commands | `make_help_command()` in `src/core/debug/commands/DebugCommands.cpp` |
| `screenshot` |  | Write the current screen to a PNG | `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp` |
| `bench` |  | Measure frame times of the current screen | `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp` |
| `dumpgameloc` |  | Write the game location screen picture to a PNG | `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp` |
| `savevga` |  | Write the current screen to a VGA file | `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp` |

### `player`

Defined by `make_player_commands()` in `src/core/debug/commands/PlayerCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `player` | `p` | - | Player management commands (group) |
| `player active` |  | `<player_id:PlayerId>` | Make a player the active one |
| `player ad_effectiveness` |  | `<channel:Int>` `<value:Int>` `[city_id:CityId]` `[player_id:PlayerId]` | Set one advertising channel's effectiveness in a city |
| `player balance` |  | `[player_id:PlayerId]` | Print a player's balance sheet |
| `player campaigns` |  | `[player_id:PlayerId]` | List a player's running advertising campaigns |
| `player competition` |  | `<willingness:Int>` `<wins:Int>` `[player_id:PlayerId]` | Set an AI player's competition willingness and wins |
| `player holiday` | `vacation` | `<days:Int>` `[player_id:PlayerId]` | Send a player on holiday |
| `player info` |  | `[player_id:PlayerId]` | Show player information |
| `player inventory` | `inv` | - | Manage player's inventory (group) |
| `player inventory buy` |  | `<item_id:ItemId>` `<amount:Int>` `[player_id:PlayerId]` | Buy weapons at the active city's price |
| `player inventory clear` |  | `[player_id:PlayerId]` | Clear all items from inventory |
| `player jail` | `prison` | `<days:Int>` `[player_id:PlayerId]` | Send a player to prison |
| `player loan` |  | `<action:String>` `[amount:Money]` | Take, pay or repay a bank loan |
| `player money` |  | - | Manage player's cash balance (group) |
| `player money add` |  | `<amount:Money>` `[player_id:PlayerId]` | Add money to player's cash |
| `player money set` |  | `<amount:Money>` `[player_id:PlayerId]` | Set player's cash to specific amount |
| `player money sub` | `subtract`, `remove` | `<amount:Money>` `[player_id:PlayerId]` | Subtract money from player's cash |
| `player recipes` |  | - | Manage player's pizza recipes (group) |
| `player recipes export` |  | `<recipe_id:RecipeId>` `<file:String>` `[player_id:PlayerId]` | Export a recipe to a recipe file |
| `player recipes import` |  | `<file:String>` `[player_id:PlayerId]` | Import a recipe file as a user recipe |
| `player recipes load` |  | `[player_id:PlayerId]` `[index:Int]` | Load standard pizza recipes |
| `player recipes show` |  | `<recipe_id:RecipeId>` `[player_id:PlayerId]` | List a recipe's toppings |
| `player startofday` |  | `<hour:Int>` `[player_id:PlayerId]` | Set the hour a player's next day starts |
| `player weapon_profit` |  | `<amount:Money>` `[player_id:PlayerId]` | Set the player's weapon dealing profit |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `player active` | `player_id` | PlayerId | yes | min 0; max NUMBER_OF_PLAYERS - 1 | Player ID |
| `player ad_effectiveness` | `channel` | Int | yes | min 0; max 3 | Advertising channel (0-3) |
| `player ad_effectiveness` | `value` | Int | yes | min 0; max 255 | Effectiveness (0-255) |
| `player ad_effectiveness` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `player ad_effectiveness` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player balance` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player campaigns` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player competition` | `willingness` | Int | yes | min 0; max 255 | Willingness (0-255) |
| `player competition` | `wins` | Int | yes | min 0; max 255 | Competition wins (0-255) |
| `player competition` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player holiday` | `days` | Int | yes | min 1; max 99 | Days of holiday |
| `player holiday` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player info` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player inventory buy` | `item_id` | ItemId | yes |  | Item ID |
| `player inventory buy` | `amount` | Int | yes | min 1; max 255 | Quantity to buy (1-255) |
| `player inventory buy` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player inventory clear` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player jail` | `days` | Int | yes | min 1; max 99 | Days of the sentence |
| `player jail` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player loan` | `action` | String | yes | one of: `take`, `month`, `repay` | take, month or repay |
| `player loan` | `amount` | Money | no |  | Loan amount for take |
| `player money add` | `amount` | Money | yes | min 0 | Dollar amount to add |
| `player money add` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player money set` | `amount` | Money | yes | min 0 | Dollar amount to set |
| `player money set` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player money sub` | `amount` | Money | yes | min 0 | Dollar amount to subtract |
| `player money sub` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player recipes export` | `recipe_id` | RecipeId | yes |  | Recipe to export |
| `player recipes export` | `file` | String | yes |  | Path to write the recipe file to |
| `player recipes export` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player recipes import` | `file` | String | yes |  | Path to the recipe file |
| `player recipes import` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player recipes load` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player recipes load` | `index` | Int | no | min 0; max core::food::STANDARD_PIZZA_COUNT - 1 | "Optional recipe index (0-" + std::to_string ( core::food::STANDARD_PIZZA_COUNT - 1 ) + ")" |
| `player recipes show` | `recipe_id` | RecipeId | yes |  | Recipe to show |
| `player recipes show` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player startofday` | `hour` | Int | yes | min 0; max Player::default_start_of_day_hour | Hour of the day to start at |
| `player startofday` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `player weapon_profit` | `amount` | Money | yes |  | Dollar amount |
| `player weapon_profit` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |

Usage lines from the help text:

* `player active <player_id>`
* `player ad_effectiveness <channel> <value> [city_id] [player_id]`
* `player balance [player_id]`
* `player campaigns [player_id]`
* `player competition <willingness> <wins> [player_id]`
* `player holiday <days> [player_id]`
* `player info [player_id]`
* `player inventory buy <item_id> <amount> [player_id]`
* `player inventory clear [player_id]`
* `player jail <days> [player_id]`
* `player loan <take|month|repay> [amount]`
* `player money add <amount> [player_id]`
* `player money set <amount> [player_id]`
* `player money sub <amount> [player_id]`
* `player recipes export <recipe_id> <file> [player_id]`
* `player recipes import <file> [player_id]`
* `player recipes load [player_id] [index]`
* `player recipes show <recipe_id> [player_id]`
* `player startofday <hour> [player_id]`
* `player weapon_profit <amount> [player_id]`

### `restaurant`

Defined by `make_restaurant_commands()` in `src/core/debug/commands/RestaurantCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `restaurant` | `r`, `rest` | - | Restaurant management commands (group) |
| `restaurant ai-layout` |  | `<index:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Replace a restaurant's layout with a prebuilt AI one |
| `restaurant clear` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Remove all furniture from restaurant |
| `restaurant close` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Close a restaurant for business (see shutdown to get rid of the restaurant) |
| `restaurant contract` |  | `<supplier_id:Int>` `<ingredient_id:Int>` `<kg_per_day:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Sign a supply contract with a supplier |
| `restaurant create` |  | `[city_id:CityId]` `[player_id:PlayerId]` `<layout_id:Int>` | Create a new restaurant with layout |
| `restaurant furniture` |  | - | Manage restaurant furniture (group) |
| `restaurant furniture list` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | List all furniture in restaurant |
| `restaurant info` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Show restaurant information |
| `restaurant inspection-timer` |  | `<days:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Set the mayor inspection countdown on a restaurant |
| `restaurant open` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Open a restaurant for business |
| `restaurant qs` |  | `[city_id:CityId]` `[player_id:PlayerId]` | Quick start: Create fully functional restaurant |
| `restaurant shutdown` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Shut a restaurant down and give up its premise |
| `restaurant stock` |  | - | Manage restaurant ingredient stocks (group) |
| `restaurant stock buy` |  | `<supplier_id:Int>` `<ingredient_id:Int>` `<kg:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Buy stock for one ingredient from a supplier |
| `restaurant stock clear` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Clear all ingredient stocks |
| `restaurant stock fill` |  | `<kg:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Buy the same weight of every ingredient sold |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `restaurant ai-layout` | `index` | Int | yes | min 1; max RestaurantLayoutFactory::ai_layout_count | AI layout number |
| `restaurant ai-layout` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant ai-layout` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant clear` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant clear` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant close` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant close` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant contract` | `supplier_id` | Int | yes | min 0; max 255 | Supplier id, from `city suppliers` |
| `restaurant contract` | `ingredient_id` | Int | yes | min 0; max 89 | Ingredient ID (0-89) |
| `restaurant contract` | `kg_per_day` | Int | yes | min 1; max 650 | Kilograms delivered each day |
| `restaurant contract` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant contract` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant create` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `restaurant create` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant create` | `layout_id` | Int | yes | min 1; max 8 | Layout ID (1-8) |
| `restaurant furniture list` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant furniture list` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant info` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant info` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant inspection-timer` | `days` | Int | yes | min 0; max 365 | Days until next inspection (0 = next EOD) |
| `restaurant inspection-timer` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant inspection-timer` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant open` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant open` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant qs` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `restaurant qs` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant shutdown` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant shutdown` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant stock buy` | `supplier_id` | Int | yes | min 0; max 255 | Supplier id, from `city suppliers` |
| `restaurant stock buy` | `ingredient_id` | Int | yes | min 0; max 89 | Ingredient ID (0-89) |
| `restaurant stock buy` | `kg` | Int | yes | min 1; max 650 | Kilograms to buy |
| `restaurant stock buy` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant stock buy` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant stock clear` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant stock clear` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `restaurant stock fill` | `kg` | Int | yes | min 1; max 650 | Kilograms of each ingredient |
| `restaurant stock fill` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `restaurant stock fill` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |

Usage lines from the help text:

* `restaurant ai-layout <index> [player_id] [restaurant_id]`
* `restaurant clear [player_id] [restaurant_id]`
* `restaurant close [player_id] [restaurant_id]`
* `restaurant contract <supplier_id> <ingredient_id> <kg_per_day> [player_id] [restaurant_id]`
* `restaurant create <city_id> <player_id> <layout_id>`
* `restaurant furniture list [player_id] [restaurant_id]`
* `restaurant info [player_id] [restaurant_id]`
* `restaurant inspection-timer <days> [player_id] [restaurant_id]`
* `restaurant open [player_id] [restaurant_id]`
* `restaurant qs <city_id> <player_id>`
* `restaurant shutdown [player_id] [restaurant_id]`
* `restaurant stock buy <supplier_id> <ingredient_id> <kg> [player_id] [restaurant_id]`
* `restaurant stock clear [player_id] [restaurant_id]`
* `restaurant stock fill <kg> [player_id] [restaurant_id]`

### `depot`

Defined by `make_depot_commands()` in `src/core/debug/commands/DepotCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `depot` | `d`, `dep` | - | Depot management commands (group) |
| `depot give` |  | `[city_id:CityId]` `[player_id:PlayerId]` | Give player a free depot in the city |
| `depot info` |  | `[city_id:CityId]` `[player_id:PlayerId]` | Show depot information |
| `depot stock` |  | - | Manage depot ingredient stocks (group) |
| `depot stock add` |  | `<ingredient_id:Int>` `[quantity:Int]` `[quality:Int]` `[city_id:CityId]` `[player_id:PlayerId]` | Add stock for a specific ingredient |
| `depot stock clear` |  | `[city_id:CityId]` `[player_id:PlayerId]` | Clear all ingredient stocks |
| `depot stock fill` |  | `<kg:Int>` `[city_id:CityId]` `[player_id:PlayerId]` | Buy stock for every ingredient the city's suppliers sell |
| `depot stock steal` |  | `[city_id:CityId]` `[player_id:PlayerId]` | Steal a random  ingredient stock category |
| `depot weapons` |  | `<item_id:ItemId>` `<amount:Int>` `[city_id:CityId]` `[player_id:PlayerId]` `[depot:Int]` | Move weapons between the player's bag and a depot |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `depot give` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot give` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot info` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot info` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot stock add` | `ingredient_id` | Int | yes | min 0; max 89 | Ingredient ID (0-89) |
| `depot stock add` | `quantity` | Int | no | min 0; max 1000000 | Quantity in grams |
| `depot stock add` | `quality` | Int | no | min 0; max 100 | Quality 0-100 |
| `depot stock add` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot stock add` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot stock clear` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot stock clear` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot stock fill` | `kg` | Int | yes | min 1; max 650 | Kilograms of each ingredient |
| `depot stock fill` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot stock fill` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot stock steal` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot stock steal` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot weapons` | `item_id` | ItemId | yes |  | Weapon item ID (0-7) |
| `depot weapons` | `amount` | Int | yes | min 0; max 255 | Quantity to store, or 0 to take stock back |
| `depot weapons` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `depot weapons` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `depot weapons` | `depot` | Int | no | min 0 | Which of the player's depots in the city |

Usage lines from the help text:

* `depot give [city_id] [player_id]`
* `depot info [city_id] [player_id]`
* `depot stock add <ingredient_id> [quantity] [quality] [city_id] [player_id]`
* `depot stock clear [city_id] [player_id]`
* `depot stock fill <kg> [city_id] [player_id]`
* `depot stock steal [city_id] [player_id]`
* `depot weapons <item_id> <amount> [city_id] [player_id] [depot]`

### `game`

Defined by `make_game_commands()` in `src/core/debug/commands/GameCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `game` |  | - | Game state management (group) |
| `game clock` |  | - | Clock state |
| `game competition` |  | - | Pizza competition commands (group) |
| `game competition monthly` |  | - | Trigger the monthly pizza competition |
| `game date` |  | - | Date management (runs, and has sub-commands) |
| `game date set` |  | `<date:String>` | Set the game date |
| `game end` |  | `<cause:String>` | Trigger game over for the active player |
| `game exit` | `quit` | - | Exit the game |
| `game levent` |  | `<id:Int>` | Fire a life event (CV milestone) for the active player |
| `game load` |  | `<filename:String>` | Load a game from a file |
| `game mission` |  | - | Mission mode target CV and scoring (runs, and has sub-commands) |
| `game mission write` |  | `<name:String>` | Write the active player's CV as a mission file |
| `game options` |  | - | Show the game options (runs, and has sub-commands) |
| `game options set` |  | `<name:String>` `<value:String>` | Change an option for this run |
| `game realtime` |  | `<mode:String>` | Run the clock as if the player were on a live screen |
| `game save` |  | `<filename:String>` | Save the game to a file |
| `game tick` |  | `[count:Int]` | Advance the simulation by whole ticks |
| `game time` |  | - | Time of day management (runs, and has sub-commands) |
| `game time set` |  | `<time:String>` | Set the time of day |
| `game timescale` |  | `[scale:Int]` | Show or set the fast-forward multiplier |
| `game turn` |  | - | Turn control (group) |
| `game turn end` |  | - | End the active player's turn |
| `game turn queue` |  | - | List the mayor's and police's queued calls |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `game date set` | `date` | String | yes |  | Date in dd-mm-yyyy format |
| `game end` | `cause` | String | yes | one of: `debts`, `killed`, `suppliers`, `tycoon` | Game over cause |
| `game levent` | `id` | Int | yes |  | compat::format ( "MilestoneId to fire (1-{})" , core::events::max_milestone_id ) |
| `game load` | `filename` | String | yes |  | Filename to load from (.plsave added if needed) |
| `game mission write` | `name` | String | yes |  | Mission filename (.mis added if needed) |
| `game options set` | `name` | String | yes |  | Option name, as listed by `game options` |
| `game options set` | `value` | String | yes |  | New value for the option |
| `game realtime` | `mode` | String | yes | one of: `on`, `off` | on or off |
| `game save` | `filename` | String | yes |  | Filename to save to (.plsave added if needed) |
| `game tick` | `count` | Int | no |  | Number of ticks (default 1) |
| `game time set` | `time` | String | yes |  | Time in hh:mm format |
| `game timescale` | `scale` | Int | no |  | Speed multiplier (1-10) |

Usage lines from the help text:

* `game competition monthly`
* `game date set <dd-mm-yyyy>`
* `game end <cause>`
* `game exit`
* `game load <filename>`
* `game mission`
* `game mission write <name>`
* `game options`
* `game options set <name> <value>`
* `game realtime <on|off>`
* `game save <filename>`
* `game tick [n]`
* `game time set <hh:mm>`
* `game timescale [n]`
* `game turn end`
* `game turn queue`

### `nav`

Defined by `make_nav_commands()` in `src/core/debug/commands/NavCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `nav` | `ns` | - | Navigation commands (group) |
| `nav advertising` | `ad` | - | advertising |
| `nav cityview` | `city`, `cv` | `[city_id:CityId]` | Navigate to city view |
| `nav demographics` |  | - | Furniture & customers page of the active restaurant |
| `nav depot` |  | `[category:String]` | Navigate to the central depot |
| `nav disk` | `dm` | - | disk menu |
| `nav euromap` | `em` | - | Europe map |
| `nav gameloc` |  | - | game location |
| `nav globalstats` | `gs` | `[screen:Int]` | Navigate to the company statistics screens |
| `nav intro` | `is` | - | intro screen |
| `nav load` |  | - | game load menu |
| `nav main` | `mm` | - | main menu |
| `nav ng` |  | - | new game menu |
| `nav op` |  | `[section:String]` | Navigate to the options menu |
| `nav order` |  | `<category:String>` `<supplier_id:Int>` `[depot:String]` | Navigate to an ingredient order screen |
| `nav pizza` |  | - | Pizza designer/recipes (group) |
| `nav pizza design` |  | - | Pizza design screen |
| `nav pizza export` |  | `<recipe_id:Int>` | Recipe export screen |
| `nav pizza import` |  | - | Recipe import screen |
| `nav pizza recipes` |  | - | Recipe list screen |
| `nav profile` |  | `[player_id:PlayerId]` | Navigate to a player's profile |
| `nav purchasing` | `pm` | - | Purchasing screen |
| `nav restaurant` | `rest`, `rv` | `[restaurant_id:RestaurantId]` | Navigate to restaurant view |
| `nav stats` |  | `[restaurant_id:RestaurantId]` `[screen:Int]` | Navigate to a restaurant's statistics screens |
| `nav stock` | `st` | `[category:String]` | Stock management |
| `nav suppliers` | `sl` | - | Supplier list |
| `nav terminal` | `tm` | - | Terminal |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `nav cityview` | `city_id` | CityId | no | min 0; max NUMBER_OF_CITIES - 1; default: ActiveCity | City ID (@ for active city) |
| `nav depot` | `category` | String | no | one of: `vegetables`, `meat`, `fruit`, `cheese` | vegetables, meat, fruit or cheese |
| `nav globalstats` | `screen` | Int | no | min 0; max 7 | Which statistics screen to open |
| `nav op` | `section` | String | no | one of: `audio`, `graphics`, `controls`, `gameplay`, `rules`, `bindings` | audio, graphics, controls, gameplay, rules or bindings |
| `nav order` | `category` | String | yes | one of: `vegetables`, `meat`, `fruit`, `cheese` | vegetables, meat, fruit or cheese |
| `nav order` | `supplier_id` | Int | yes | min 0; max 255 | Which supplier of that category |
| `nav order` | `depot` | String | no |  | Say "depot" for the central depot's screen |
| `nav pizza export` | `recipe_id` | Int | yes |  | Recipe id |
| `nav profile` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `nav restaurant` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `nav stats` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `nav stats` | `screen` | Int | no | min 0; max 5 | Which statistics screen to open |
| `nav stock` | `category` | String | no | one of: `vegetables`, `meat`, `fruit`, `cheese` | vegetables, meat, fruit or cheese |

Usage lines from the help text:

* `nav cityview <city_id>`
* `nav depot [category]`
* `nav globalstats [screen]`
* `nav op [section]`
* `nav order <category> <supplier_id> [depot]`
* `nav pizza <design|recipes|export|import>`
* `nav pizza export <recipe_id>`
* `nav profile [player_id]`
* `nav restaurant <restaurant_id>`
* `nav stats <restaurant_id> [screen]`
* `nav stock [category]`

### `debug`

Defined by `make_debug_commands()` in `src/core/debug/commands/DebugCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `debug` |  | - | Debug and diagnostic commands (group) |
| `debug bindings` |  | - | Load or save key bindings files (group) |
| `debug bindings load` |  | `<file:String>` | Apply a bindings file |
| `debug bindings save` |  | `<file:String>` | Write the current bindings to a file |
| `debug dump` |  | - | Dump debugging information (group) |
| `debug dump fingerprint` |  | `[file:String]` | Print a hash of the entire game state |
| `debug dump input` |  | `<type:String>` | Dump input system state |
| `debug dump inventory` |  | `[player_id:PlayerId]` | Dump player inventory |
| `debug dump layers` |  | - | Dump interaction layers |
| `debug dump ledger` |  | `[player_id:PlayerId]` | Dump player financials |
| `debug dump player` |  | `[player_id:PlayerId]` | Dump player information |
| `debug dump players` |  | - | Dump all players |
| `debug dump state` |  | - | Dump entire game state |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `debug bindings load` | `file` | String | yes |  | Bindings file |
| `debug bindings save` | `file` | String | yes |  | Bindings file |
| `debug dump fingerprint` | `file` | String | no |  | Where to write the hashed document |
| `debug dump input` | `type` | String | yes | one of: `bindings`, `state` | What to dump (bindings or state) |
| `debug dump inventory` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `debug dump ledger` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `debug dump player` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |

Usage lines from the help text:

* `debug bindings load <file>`
* `debug bindings save <file>`
* `dump fingerprint [file]`
* `debug dump input <bindings|state>`
* `debug dump inventory [player_id]`
* `debug dump layers`
* `debug dump ledger [player_id]`
* `debug dump player [player_id]`
* `debug dump players`
* `debug dump state`

### `news`

Defined by `make_newspaper_commands()` in `src/core/debug/commands/NewspaperCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `news` |  | - | Newspaper and trend system (group) |
| `news article` |  | - | Manage newspaper article display state (group) |
| `news article clear` |  | - | Reset article to none |
| `news article ingredient` |  | `<slot:Int>` `<polarity:Int>` `<category:Int>` | Set an ingredient article |
| `news article read` |  | - | Mark article as read (cooldown=0) |
| `news article set` |  | `<article_id:Int>` `[cooldown:Int]` | Overwrite current article |
| `news article show` |  | - | Dump current NewspaperState fields |
| `news schedule` |  | - | Manage scheduled newspaper events (group) |
| `news schedule clear` |  | `[slot:Int]` | Clear one or all scheduled slots |
| `news schedule fire` |  | `<event_id:Int>` | Immediately execute a named event |
| `news schedule generate` | `gen` | - | Generate a random ingredient trend event |
| `news schedule list` |  | `[all:String]` | Show scheduled event slots |
| `news schedule set` |  | `<slot:Int>` `<event_id:Int>` `<days:Int>` | Schedule an event into a slot |
| `news status` |  | - | Show newspaper status overview |
| `news trends` |  | - | Manage ingredient trend system (group) |
| `news trends clear` |  | `[slot:Int]` | Clear one or all trend slots |
| `news trends get` |  | `<slot:Int>` | Full detail for one trend slot |
| `news trends list` |  | `[all:String]` | Show active ingredient trends |
| `news trends set` |  | `<slot:Int>` `<initial:Int>` `<target:Int>` `<duration:String>` | Apply a TrendEffect to a slot |
| `news trends tick` |  | `[n:Int]` | Advance trend system by n days |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `news article ingredient` | `slot` | Int | yes |  | Ingredient slot (0-95) |
| `news article ingredient` | `polarity` | Int | yes |  | +1 or -1 |
| `news article ingredient` | `category` | Int | yes |  | Category (0-3) |
| `news article set` | `article_id` | Int | yes |  | Article ID |
| `news article set` | `cooldown` | Int | no |  | Cooldown days (default: 4) |
| `news schedule clear` | `slot` | Int | no |  | Slot index (0-49), or omit for all |
| `news schedule fire` | `event_id` | Int | yes |  | Event ID (1-51) |
| `news schedule list` | `all` | String | no |  | Pass 'all' to show all 50 slots |
| `news schedule set` | `slot` | Int | yes |  | Slot index (0-49) |
| `news schedule set` | `event_id` | Int | yes |  | Event ID (1-51) |
| `news schedule set` | `days` | Int | yes |  | Days until event fires |
| `news trends clear` | `slot` | Int | no |  | Slot index (0-95), or omit for all |
| `news trends get` | `slot` | Int | yes |  | Ingredient slot (0-95) |
| `news trends list` | `all` | String | no |  | Pass 'all' to show all 96 slots |
| `news trends set` | `slot` | Int | yes |  | Ingredient slot (0-95) |
| `news trends set` | `initial` | Int | yes |  | Initial value (-128 to 127) |
| `news trends set` | `target` | Int | yes |  | Target value (-128 to 127) |
| `news trends set` | `duration` | String | yes |  | none/linear/exponential/countdown |
| `news trends tick` | `n` | Int | no |  | Number of days to advance (default: 1) |

Usage lines from the help text:

* `news article clear`
* `news article ingredient <slot> <polarity> <category>`
* `news article read`
* `news article set <article_id> [cooldown]`
* `news article show`
* `news schedule clear [slot]`
* `news schedule fire <event_id>`
* `news schedule generate`
* `news schedule list [all]`
* `news schedule set <slot> <event_id> <days>`
* `news status`
* `news trends clear [slot]`
* `news trends get <slot>`
* `news trends list [all]`
* `news trends set <slot> <initial> <target> <duration>`
* `news trends tick [n]`

### `city`

Defined by `make_city_commands()` in `src/core/debug/commands/CityCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `city` |  | - | City-level commands (group) |
| `city chart` | `charts` | - | Pizza chart commands (group) |
| `city chart debug` |  | `[player_id:PlayerId]` | Show ALL chart candidates with scores |
| `city chart rebuild` |  | - | Force rebuild the pizza chart now |
| `city chart show` |  | - | Show current pizza chart (top 10) |
| `city click` |  | `<index:Int>` | Select a map tile as a click at close zoom does |
| `city districts` |  | `[city_id:CityId]` | List the active city's districts |
| `city investigation` |  | `<district_id:Int>` | Set the district id the police are investigating |
| `city property` | `properties` | `[index:Int]` | Show premises with their price, rent and realtor |
| `city rent` |  | `<index:Int>` | Rent a premise for the active player |
| `city suppliers` |  | - | List the active city's food suppliers |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `city chart debug` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1 | Filter to specific player (@ for active) |
| `city click` | `index` | Int | yes | min 0; max MAP_TILES_PER_ROW * MAP_TILES_PER_COLUMN - 1 | Map index of the tile |
| `city districts` | `city_id` | CityId | no | min 0; max 9; default: ActiveCity | City ID (@ for active city) |
| `city investigation` | `district_id` | Int | yes | min 0; max 255 | District id to investigate |
| `city property` | `index` | Int | no |  | Map index of a single premise |
| `city rent` | `index` | Int | yes | min 0; max MAP_TILES_PER_ROW * MAP_TILES_PER_COLUMN - 1 | Map index of the premise |

Usage lines from the help text:

* `city chart debug [player_id]`
* `city chart rebuild`
* `city chart show`
* `city click <index>`
* `city districts [city_id]`
* `city investigation <district_id>`
* `city property [index]`
* `city rent <index>`
* `city suppliers`

### `mafia`

Defined by `make_mafia_commands()` in `src/core/debug/commands/ActorCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `mafia` |  | - | Mafia interaction commands (group) |
| `mafia loan-due` |  | - | Trigger mafia loan repayment event |
| `mafia mission-overdue` |  | - | Trigger mafia mission overdue event |
| `mafia set-bust` |  | `<busted:Bool>` | Toggle the busted flag on the active mission |
| `mafia set-deadline` |  | `<days:Int>` | Set the mission deadline to N days from now |
| `mafia set-dropoff` |  | `<tile_index:Int>` `[hour:Int]` `[minute:Int]` | Override dropoff tile and time |
| `mafia set-pickup` |  | `<tile_index:Int>` `[hour:Int]` `[minute:Int]` | Override pickup tile and time |
| `mafia shakedown` |  | `[restaurant_id:RestaurantId]` `[when:String]` | Trigger a mafia protection racket visit |
| `mafia status` |  | - | Show mafia-related player state |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `mafia set-bust` | `busted` | Bool | yes |  | true or false |
| `mafia set-deadline` | `days` | Int | yes |  | Days from now |
| `mafia set-dropoff` | `tile_index` | Int | yes |  | Tile index |
| `mafia set-dropoff` | `hour` | Int | no |  | Hour (0-23), default: current hour |
| `mafia set-dropoff` | `minute` | Int | no |  | Minute (0-59), default: current minute |
| `mafia set-pickup` | `tile_index` | Int | yes |  | Tile index |
| `mafia set-pickup` | `hour` | Int | no |  | Hour (0-23), default: current hour |
| `mafia set-pickup` | `minute` | Int | no |  | Minute (0-59), default: current minute |
| `mafia shakedown` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `mafia shakedown` | `when` | String | no | one of: `now`, `queue` | now or queue (default: now) |

Usage lines from the help text:

* `actor mafia loan-due`
* `actor mafia mission-overdue`
* `actor mafia set-bust <true|false>`
* `actor mafia set-deadline <days>`
* `actor mafia set-dropoff <tile_index> [hour] [minute]`
* `actor mafia set-pickup <tile_index> [hour] [minute]`
* `mafia shakedown [restaurant_id] [when]`
* `actor mafia status`

### `police`

Defined by `make_police_commands()` in `src/core/debug/commands/ActorCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `police` |  | - | Police interaction commands (group) |
| `police arrest` |  | `<crime_code:Int>` | Trigger a police arrest with a given crime code |
| `police raids` |  | - | List depot raids waiting to be told to their owners |
| `police seize` |  | - | Seize the weapons a player is carrying |
| `police status` |  | - | Show player's criminal record and jail status |
| `police tip` |  | - | Show what the mayor-visit tip would say |
| `police visit` |  | - | Trigger a voluntary police station visit |
| `police warehouse` |  | - | Show what the warehouse inspection tip would say |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `police arrest` | `crime_code` | Int | yes | min lowest_crime_code; max highest_crime_code | compat::format ( "Crime code ({}-{})" , lowest_crime_code , highest_crime_code ) |

Usage lines from the help text:

* `police raids`
* `police seize`
* `police status`
* `police tip`
* `police visit`
* `police warehouse`

### `bank`

Defined by `make_bank_commands()` in `src/core/debug/commands/ActorCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `bank` |  | - | Bank interaction commands (group) |
| `bank loan` |  | `[bank:Int]` `[accept:Bool]` | Ask a bank in the active city for a loan |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `bank loan` | `bank` | Int | no | min 0 | Bank number in the active city |
| `bank loan` | `accept` | Bool | no |  | Accept the offer |

Usage lines from the help text:

* `bank loan [bank] [accept]`

### `mayor`

Defined by `make_mayor_commands()` in `src/core/debug/commands/MayorCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `mayor` |  | - | Mayor interaction commands (group) |
| `mayor award` |  | `<type:String>` `[winner_player_id:Int]` `[when:String]` | Trigger or queue a mayor award |
| `mayor favor` |  | `<amount:Int>` `[player_id:PlayerId]` | Set the mayor favor score for a player |
| `mayor fee` |  | `[player_id:PlayerId]` | Mark the award participation fee as paid |
| `mayor inspect` |  | `[restaurant_id:RestaurantId]` `[first:Bool]` | Queue and trigger a mayor restaurant inspection |
| `mayor status` |  | - | Show mayor office state |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `mayor award` | `type` | String | yes | one of: `beauty`, `pizza`, `investor`, `imitator`, `bowtie`, `trade` | Award type |
| `mayor award` | `winner_player_id` | Int | no | min -1; max NUMBER_OF_PLAYERS - 1 | Winner player ID (-1 for announcement only) |
| `mayor award` | `when` | String | no | one of: `now`, `queue` | now or queue (default: now) |
| `mayor favor` | `amount` | Int | yes | min 0; max 200 | Favor score (0-200) |
| `mayor favor` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `mayor fee` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `mayor inspect` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `mayor inspect` | `first` | Bool | no |  | true = first-time inspection (default false) |

Usage lines from the help text:

* `mayor award <type> [winner_player_id] [when]`
* `mayor favor <amount> [player_id]`
* `mayor fee [player_id]`
* `mayor inspect [restaurant_id] [first]`
* `mayor status`

### `staff`

Defined by `make_staff_commands()` in `src/core/debug/commands/StaffCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `staff` | `st` | - | Staff management and event testing commands (group) |
| `staff event` |  | `<type:String>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` `[slot:Int]` `[when:String]` | Trigger a staff event dialog directly |
| `staff finish_training` |  | `[days:Int]` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Shorten active training courses for fast testing |
| `staff hire` |  | `<role:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` `[character_id:Int]` | Hire character into a restaurant's staff slot |
| `staff list` |  | `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | List all staff in a restaurant |
| `staff manager` |  | `<setting:String>` `<value:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` | Set one of the manager settings |
| `staff set_days_away` |  | `<days:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` `[slot:Int]` | Set the absence counter for a staff member |
| `staff set_motivation` |  | `<value:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` `[slot:Int]` | Set a staff member's motivation directly |
| `staff train` |  | `<course:Int>` `[player_id:PlayerId]` `[restaurant_id:RestaurantId]` `[slot:Int]` | Send a staff member on a training course |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `staff event` | `type` | String | yes | one of: `late`, `sick`, `maternity`, `quit`, `raise`, `theft`, `destroycheesestock`, `blowupoven`, `spillwine`, `messeduporders`, `forgottoopen` | Event type |
| `staff event` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff event` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff event` | `slot` | Int | no | min 0; max 12 | Staff slot (0-5 cooks, 6-11 waiters, 12 manager) |
| `staff event` | `when` | String | no | one of: `now`, `queue` | now or queue (default: now) |
| `staff finish_training` | `days` | Int | no | min 1; max 365 | Days left on the course (>= 1) |
| `staff finish_training` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff finish_training` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff hire` | `role` | Int | yes | min 0; max 2 | 0 cook, 1 waiter, 2 manager |
| `staff hire` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff hire` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff hire` | `character_id` | Int | no | min 0; max 199 | Character to take on (0-199) |
| `staff list` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff list` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff manager` | `setting` | String | yes | one of: `shopping`, `prices`, `overdraw`, `turnover_down`, `turnover_up`, `supply`, `staff`, `behaviour` | Which setting |
| `staff manager` | `value` | Int | yes | min 0; max 6 | The setting's value |
| `staff manager` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff manager` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff set_days_away` | `days` | Int | yes | min 0; max 365 | Days away (0 to return immediately) |
| `staff set_days_away` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff set_days_away` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff set_days_away` | `slot` | Int | no | min 0; max 12 | Staff slot (0-5 cooks, 6-11 waiters, 12 manager) |
| `staff set_motivation` | `value` | Int | yes | min 0; max 100 | Target motivation (0-100) |
| `staff set_motivation` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff set_motivation` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff set_motivation` | `slot` | Int | no | min 0; max 12 | Staff slot (0-5 cooks, 6-11 waiters, 12 manager) |
| `staff train` | `course` | Int | yes | min 0; max 17 | Course index (0-17) |
| `staff train` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `staff train` | `restaurant_id` | RestaurantId | no | default: ActiveRestaurant | Restaurant ID (@ for active restaurant) |
| `staff train` | `slot` | Int | no | min 0; max 12 | Staff slot (0-5 cooks, 6-11 waiters, 12 manager) |

Usage lines from the help text:

* `staff event <type> [player_id] [restaurant_id] [slot]`
* `staff finish_training [days] [player_id] [restaurant_id]`
* `staff hire <role> [player_id] [restaurant_id] [character_id]`
* `staff list [player_id] [restaurant_id]`
* `staff manager <setting> <value> [player_id] [restaurant_id]`
* `staff set_days_away <days> [player_id] [restaurant_id] [slot]`
* `staff set_motivation <value> [player_id] [restaurant_id] [slot]`
* `staff train <course> [player_id] [restaurant_id] [slot]`

### `armsdealer`

Defined by `make_armsdealer_commands()` in `src/core/debug/commands/ActorCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `armsdealer` |  | - | Arms dealer commands (group) |
| `armsdealer market` |  | - | Print weapon buy/sell prices for every city |

Usage lines from the help text:

* `armsdealer market`

### `tutorial`

Defined by `make_tutorial_commands()` in `src/core/debug/commands/TutorialCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `tutorial` |  | - | Tutorial commands (group) |
| `tutorial next` |  | - | Finish the current step as if the player had |
| `tutorial skip` |  | - | Leave the tutorial and play on |
| `tutorial start` |  | `[seed:Int]` | Start a new game in the tutorial |
| `tutorial status` |  | - | Show the tutorial's current step |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `tutorial start` | `seed` | Int | no |  | World seed (default: the tutorial's own) |

Usage lines from the help text:

* `tutorial next`
* `tutorial skip`
* `tutorial start [seed]`
* `tutorial status`

### `dump`

Defined by `make_dump_command()` in `src/core/debug/commands/DebugCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `dump` |  | - | Dump debugging information (alias for 'debug dump') (group) |
| `dump fingerprint` |  | `[file:String]` | Print a hash of the entire game state |
| `dump input` |  | `<type:String>` | Dump input system state |
| `dump inventory` |  | `[player_id:PlayerId]` | Dump player inventory |
| `dump layers` |  | - | Dump interaction layers |
| `dump ledger` |  | `[player_id:PlayerId]` | Dump player financials |
| `dump player` |  | `[player_id:PlayerId]` | Dump player information |
| `dump players` |  | - | Dump all players |
| `dump state` |  | - | Dump entire game state |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `dump fingerprint` | `file` | String | no |  | Where to write the hashed document |
| `dump input` | `type` | String | yes | one of: `bindings`, `state` | What to dump (bindings or state) |
| `dump inventory` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `dump ledger` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |
| `dump player` | `player_id` | PlayerId | no | min 0; max NUMBER_OF_PLAYERS - 1; default: ActivePlayer | Player ID (@ for active player) |

Usage lines from the help text:

* `dump fingerprint [file]`
* `debug dump input <bindings|state>`
* `debug dump inventory [player_id]`
* `debug dump layers`
* `debug dump ledger [player_id]`
* `debug dump player [player_id]`
* `debug dump players`
* `debug dump state`

### `help`

Defined by `make_help_command()` in `src/core/debug/commands/DebugCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `help` |  | `[command:String]` | Show help for commands |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `help` | `command` | String | no |  | Command to describe, or "all" |

Usage lines from the help text:

* `help [command]`

### `screenshot`

Defined by `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `screenshot` |  | `[file:String]` | Write the current screen to a PNG |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `screenshot` | `file` | String | no |  | Where to write the PNG |

### `bench`

Defined by `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `bench` |  | `[frames:Int]` | Measure frame times of the current screen |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `bench` | `frames` | Int | no |  | How many frames to render |

### `dumpgameloc`

Defined by `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `dumpgameloc` |  | `<file:String>` | Write the game location screen picture to a PNG |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `dumpgameloc` | `file` | String | yes |  | Where to write the PNG |

### `savevga`

Defined by `make_screen_commands()` in `src/engine/debug/ScreenCommands.cpp`.

| Command | Aliases | Arguments | Description |
|---|---|---|---|
| `savevga` |  | `<file:String>` | Write the current screen to a VGA file |

Argument details:

| Command | Argument | Type | Required | Constraints | Description |
|---|---|---|---|---|---|
| `savevga` | `file` | String | yes |  | Where to write the VGA |

## Notes

* `dump` repeats the `debug dump` sub-commands at top level (`make_dump_command()`,
  `src/core/debug/commands/DebugCommands.cpp`).
* Alias `st` is used twice at different levels: root `staff` and `nav stock`. They do not
  clash because aliases are resolved per level.
* Most `nav` commands call `GameState::bootstrap_minimal_playable_world()` first, so they work
  from the main menu or a fresh `--exec` run (`src/core/debug/commands/NavCommands.cpp`).
* `help all` prints the whole tree at run time; this file and that output come from the same
  definitions.
