/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A whole first week in Santa's Pizza Village, played without views through
// the paths a scripted run uses (the console commands of --exec and the
// core calls behind them): the Cozy Village quick start rents and furnishes
// the first pizzeria, the person rents a second premise from a realtor, puts
// the standard pizzas on the menu, stocks the kitchen, hires a cook and a
// waiter, opens, trades a day and then a week, saves, and loads the save
// into a fresh game. Needs the original game (SKIP without
// PLEGACY_TEST_GAME_PATH). The reload half needs save format version 4
// (location-contract.md section 6) and SKIPs while saves are written as
// version 3.
//
// For a reload in a separate process, set PLEGACY_JOURNEY_SAVE to a file
// path: the save test writes the save there (and its fingerprint beside it),
// and a later run of "[.journey-reload]" loads it in that new process.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "ContentTestGame.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/content/ScenarioSave.h"
#include "core/content/ShippedScenarios.h"
#include "core/debug/command/CommandRegistry.h"
#include "core/debug/command/CommandResult.h"
#include "core/debug/commands/CityCommands.h"
#include "core/debug/commands/GameCommands.h"
#include "core/debug/commands/RestaurantCommands.h"
#include "core/debug/commands/StaffCommands.h"
#include "core/factories/FurnitureFactory.h"
#include "core/food/PlayerRecipes.h"
#include "core/restaurant/Furniture.h"
#include "core/restaurant/FurniturePlacement.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"
#include "core/save/SaveGameManager.h"
#include "engine/FormatCompat.h"
#include "engine/Random.h"

namespace fs = std::filesystem;
using save::SaveGameManager;

namespace {

constexpr std::uint32_t journey_seed = 0x5A7A;

debug::command::CommandRegistry& console() {
  static debug::command::CommandRegistry registry = [] {
    debug::command::CommandRegistry r;
    r.register_command(debug::commands::make_city_commands());
    r.register_command(debug::commands::make_restaurant_commands());
    r.register_command(debug::commands::make_staff_commands());
    r.register_command(debug::commands::make_game_commands());
    return r;
  }();
  return registry;
}

debug::command::CommandResult try_run(content_test::TestGame& game,
                                      const std::string& line) {
  return console().execute(line, game.state, game.context);
}

void run(content_test::TestGame& game, const std::string& line) {
  INFO("console: " << line);
  const auto result = try_run(game, line);
  INFO((result.error_message ? *result.error_message : std::string{}));
  REQUIRE(result.status == debug::command::CommandStatus::Success);
}

// Which restaurant the person trades in.
enum class Fit {
  // The quick start's own, furnished from its layout file.
  QuickStartLayout,
  // The rented second premise, furnished piece by piece with
  // `restaurant place`.
  PieceByPiece,
};

struct Journey {
  Restaurant* shop{nullptr};
  Restaurant* second{nullptr};
  Restaurant* trading{nullptr};
};

// Two tables with a chair on each side and an oven, placed one by one by
// the placement rules the restaurant view uses. The sizes are the drawn
// ones, so the positions follow the sprites. (The game has no counter piece:
// furniture.json has tables, chairs, decor, entertainment, games, slot
// machines and kitchenware.)
void furnish_piece_by_piece(content_test::TestGame& game, Restaurant& r) {
  namespace placement = core::restaurant;
  auto& state = game.state;
  REQUIRE(r.layout());
  CHECK(r.layout()->placed_furniture().empty());
  CHECK(r.layout()->placed_kitchen_furniture().empty());
  state.active_restaurant(r.id());

  const auto size = placement::furniture_size_fn(game.bobs);
  const auto* table =
      FurnitureFactory::create_furniture(FurnitureType::ROUND_GRILL);
  const auto* chair =
      FurnitureFactory::create_furniture(FurnitureType::TRADITIO_ROMANO);
  REQUIRE(table);
  REQUIRE(chair);
  const PlacedFurniture table_probe(table, core::FurnitureId{0},
                                    WorldPoint{0, 0},
                                    core::FurnitureId{UNLINKED_FURNITURE}, 0);
  const PlacedFurniture chair_probe(chair, core::FurnitureId{0},
                                    WorldPoint{0, 0},
                                    core::FurnitureId{UNLINKED_FURNITURE}, 0);
  const Size ts = size(table_probe);
  const Size cs = size(chair_probe);
  INFO("table " << ts.w << "x" << ts.h << ", chair " << cs.w << "x" << cs.h
                << ", room " << r.size().w << "x" << r.size().h << " tiles");
  REQUIRE(ts.w > 0);
  REQUIRE(cs.w > 0);

  const int overlap = std::max(1, cs.w / 4);
  const int tx = cs.w + 4;
  for (int k = 0; k < 2; ++k) {
    const int ty = 4 + (k * (std::max(ts.h, cs.h) + 12));
    run(game, compat::format("restaurant place ROUND_GRILL {} {}", tx, ty));
    // Chairs overlap the table edge with their centre outside it, so they
    // face and join that table.
    run(game, compat::format("restaurant place TRADITIO_ROMANO {} {}",
                             tx - cs.w + overlap, ty));
    run(game, compat::format("restaurant place TRADITIO_ROMANO {} {}",
                             tx + ts.w - overlap, ty));
    // The same table again on top of the first is refused.
    CHECK(try_run(game,
                  compat::format("restaurant place ROUND_GRILL {} {}", tx, ty))
              .status != debug::command::CommandStatus::Success);
  }
  // Outside the walls is refused too.
  CHECK(try_run(game, "restaurant place ROUND_GRILL 1999 1999").status !=
        debug::command::CommandStatus::Success);
  run(game, "restaurant place OVEN_SMALL 0 0");

  const auto& dining = r.layout()->placed_furniture();
  REQUIRE(dining.size() == 6);
  REQUIRE(r.layout()->placed_kitchen_furniture().size() == 1);
  for (const auto& pf : dining) {
    if (pf->furniture()->category() == FurnitureCategory::CHAIR) {
      CHECK(pf->linked_furniture_id().has_value());
    }
  }
}

// Start, rent, furnish, menu, stock, staff and open: everything before
// trading.
Journey set_up_shop(content_test::TestGame& game,
                    Fit fit = Fit::QuickStartLayout) {
  const auto scenario = content_test::load_cozy_village();
  const auto village = content_test::load_village();
  content_test::start_village_game(game, journey_seed, scenario, village);
  auto& state = game.state;
  content_test::close_dialogs(state);

  Player& me = *state.player(core::PlayerId{0});
  REQUIRE(me.is_human_controlled());
  City& city = *state.city(core::CityId{0});
  REQUIRE(city.name() == village->name);

  Journey j;
  // The quick start rented a premise and furnished it from a layout file.
  auto mine = state.restaurants_for(me.id());
  REQUIRE(mine.size() == 1);
  j.shop = mine[0];
  CHECK(&j.shop->property().city() == &city);
  CHECK(j.shop->property().owner_or_tenant() == me.id());
  REQUIRE(j.shop->layout());
  CHECK_FALSE(j.shop->layout()->placed_furniture().empty());
  CHECK_FALSE(j.shop->layout()->placed_kitchen_furniture().empty());

  // Rent the cheapest free premise from its realtor, as the console's
  // `city rent` does in scripted runs. The person starts with the original
  // character's $8,000.
  Property* to_rent = nullptr;
  for (Property* p : city.business_premises()) {
    if (p->is_for_rent() && !p->has_restaurant() &&
        (!to_rent || p->get_monthly_rent() < to_rent->get_monthly_rent())) {
      to_rent = p;
    }
  }
  REQUIRE(to_rent != nullptr);
  const Money rent = to_rent->get_monthly_rent();
  REQUIRE(rent <= me.cash_balance());
  const Money cash_before_rent = me.cash_balance();
  run(game, "city rent " + std::to_string(to_rent->index()));
  CHECK(me.cash_balance() == cash_before_rent - rent);
  CHECK(to_rent->owner_or_tenant() == me.id());
  REQUIRE(to_rent->restaurant_id().has_value());
  j.second = state.restaurant(*to_rent->restaurant_id());
  REQUIRE(j.second != nullptr);
  CHECK(state.restaurants_for(me.id()).size() == 2);

  j.trading = j.shop;
  if (fit == Fit::PieceByPiece) {
    furnish_piece_by_piece(game, *j.second);
    j.trading = j.second;
  }

  // The quick start's standard pizzas go on the menu.
  int on_menu = 0;
  for (const auto& slot : me.recipes().standard) {
    if (slot.created()) {
      j.trading->menu().add_or_set(slot.recipe->id(), Money{800});
      ++on_menu;
    }
  }
  REQUIRE(on_menu > 0);
  CHECK(j.trading->menu().count_total() == static_cast<std::size_t>(on_menu));

  // Stock the kitchen and hire a cook and a waiter.
  const std::string rid = std::to_string(j.trading->id().value);
  const Money cash_before_stock = me.cash_balance();
  run(game, "restaurant stock fill 10 @ " + rid);
  CHECK(me.cash_balance() < cash_before_stock);
  run(game, "staff hire 0 @ " + rid + " 20");
  run(game, "staff hire 1 @ " + rid + " 21");

  if (!j.trading->wants_open()) {
    j.trading->open_for_business(state);
  }
  CHECK(j.trading->wants_open());
  return j;
}

std::string fingerprint(const content_test::TestGame& game) {
  return SaveGameManager::fingerprint(game.state, game.context).hash;
}

std::optional<fs::path> separate_process_save() {
  const char* env = std::getenv("PLEGACY_JOURNEY_SAVE");
  if (!env || !*env) {
    return std::nullopt;
  }
  return fs::path(env);
}

} // namespace

TEST_CASE(
    "Santa's Pizza Village journey: furnished piece by piece, a day and a week "
    "of trade",
    "[content][journey][game][village]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const Journey j = set_up_shop(game, Fit::PieceByPiece);
  auto& state = game.state;
  Player& me = *state.player(core::PlayerId{0});
  const GameDate first_day = state.time().current_date();

  content_test::play_day(state);
  CHECK(state.time().current_date() == first_day.add_days(1));
  CHECK(j.trading->stats().days_in_operation() >= 1);
  CHECK(me.status() == PlayerStatus::Active);

  content_test::play_days(state, 7);
  CHECK(state.time().current_date() == first_day.add_days(8));
  // The pizzeria served the village.
  CHECK(j.trading->stats().total_pizzas_sold > 0);
  CHECK(j.trading->stats().total_revenue > Money{0});
  int orders = 0;
  for (const auto& day : j.trading->stats().last_n_days(7)) {
    orders += day.orders_generated;
  }
  CHECK(orders > 0);
  CHECK(me.status() == PlayerStatus::Active);
  // The village's own AI competitors opened too, and nobody left it.
  CHECK(state.initialized_cities().size() == 1);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    for (const auto* r : state.restaurants_for(core::PlayerId{i})) {
      CHECK(r->property().city().id() == core::CityId{0});
    }
  }

  // An Expanded game is as deterministic as a Classic one: the same seed,
  // roster and inputs give the same state.
  content_test::TestGame again;
  const Journey k = set_up_shop(again, Fit::PieceByPiece);
  content_test::play_days(again.state, 8);
  CHECK(fingerprint(again) == fingerprint(game));
  CHECK(k.trading->stats().total_pizzas_sold ==
        j.trading->stats().total_pizzas_sold);
}

TEST_CASE("Santa's Pizza Village journey: the quick-start layout trades",
          "[content][journey][game][village]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const Journey j = set_up_shop(game, Fit::QuickStartLayout);
  content_test::play_days(game.state, 8);
  CHECK(j.trading == j.shop);
  CHECK(j.shop->stats().total_pizzas_sold > 0);
  CHECK(j.shop->stats().total_revenue > Money{0});
  CHECK(game.state.player(core::PlayerId{0})->status() == PlayerStatus::Active);
}

TEST_CASE("Santa's Pizza Village journey: save and reload",
          "[content][journey][game][village][save]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  set_up_shop(game);
  content_test::play_days(game.state, 3);

  test_support::TempDir dir;
  const fs::path file = dir.path() / "santas-pizza-village.plsave";
  std::string error;
  REQUIRE(
      SaveGameManager::save(game.state, game.context, file.string(), &error));
  const int version = SaveGameManager::get_save_version(file.string());
  if (version < core::content::expanded_save_version) {
    SKIP("Saves are written as version "
         << version
         << "; a game outside the original ten needs version 4 with its "
            "scenario block (location-contract.md section 6)");
  }
  const std::string saved = fingerprint(game);
  const auto rng_at_save = engine::random::get_state();

  if (const auto target = separate_process_save()) {
    fs::create_directories(target->parent_path());
    fs::copy_file(file, *target, fs::copy_options::overwrite_existing);
    std::ofstream(target->string() + ".fingerprint") << saved << '\n';
  }

  // A fresh game, with nothing of the first one but the file.
  content_test::TestGame fresh;
  engine::random::seed(0xDEAD);
  REQUIRE(
      SaveGameManager::load(fresh.state, fresh.context, file.string(), &error));
  CHECK(fingerprint(fresh) == saved);
  CHECK(engine::random::get_state() == rng_at_save);
  CHECK(fresh.state.roster() == game.state.roster());
  REQUIRE(fresh.state.scenario() != nullptr);
  CHECK(fresh.state.scenario()->id == game.state.scenario()->id);
  REQUIRE(fresh.state.city(core::CityId{0}) != nullptr);
  CHECK(fresh.state.city(core::CityId{0})->name() == "Santa's Pizza Village");

  // Both go on the same way.
  content_test::play_day(fresh.state);
  const std::string fresh_next = fingerprint(fresh);
  engine::random::set_state(rng_at_save);
  content_test::play_day(game.state);
  CHECK(fingerprint(game) == fresh_next);
}

TEST_CASE("Santa's Pizza Village journey: reload a saved game in a new process",
          "[.journey-reload][content][journey][game][village][save]") {
  CONTENT_REQUIRE_GAME_DATA();
  const auto target = separate_process_save();
  if (!target || !fs::exists(*target)) {
    SKIP("Run the save test first with PLEGACY_JOURNEY_SAVE set, then this "
         "one in a new process with the same setting");
  }
  std::ifstream in(target->string() + ".fingerprint");
  std::string expected;
  in >> expected;
  REQUIRE_FALSE(expected.empty());

  content_test::TestGame game;
  std::string error;
  REQUIRE(SaveGameManager::load(game.state, game.context, target->string(),
                                &error));
  CHECK(fingerprint(game) == expected);
  CHECK_FALSE(game.state.roster().is_classic());
  content_test::play_days(game.state, 7);
  CHECK(game.state.player(core::PlayerId{0})->status() == PlayerStatus::Active);
}

TEST_CASE("Santa's Pizza Village journey: a scripted run starts the village",
          "[content][journey][game][village]") {
  CONTENT_REQUIRE_GAME_DATA();
  const auto scenario_file =
      core::content::shipped_data_dir() / "scenarios" /
      (std::string(core::content::cozy_village.file) + ".json");
  if (!fs::exists(scenario_file)) {
    SKIP("The game's data folder ("
         << core::content::shipped_data_dir().string()
         << ") has no scenarios; run from the build "
            "folder");
  }
  // What `--exec "game scenario north-pole-cozy-village 23"` does.
  content_test::TestGame game;
  game.state.options().autosave_frequency = AutosaveFrequency::Off;
  run(game, "game scenario north-pole-cozy-village 23");
  auto& state = game.state;
  REQUIRE(state.scenario() != nullptr);
  CHECK(state.city_display_name(state.active_city_id()) ==
        "Santa's Pizza Village");
  REQUIRE(state.active_player() != nullptr);
  CHECK(state.restaurants_for(core::PlayerId{0}).size() == 1);
  content_test::close_dialogs(state);
  content_test::play_days(state, 2);
  CHECK(state.player(core::PlayerId{0})->status() == PlayerStatus::Active);
}

TEST_CASE("Every North Pole preset starts, plays a day and reloads",
          "[content][journey][game][village][save]") {
  CONTENT_REQUIRE_GAME_DATA();
  const auto village = content_test::load_village();
  for (const auto& preset : core::content::north_pole_presets) {
    INFO(preset.file);
    content_test::TestGame game;
    const auto scenario = content_test::load_preset(preset.file);
    content_test::start_village_game(game, 0x4E50, scenario, village);
    auto& state = game.state;
    REQUIRE(state.scenario() == scenario.get());
    // Quick start brings a pizzeria; the free-mode presets start empty.
    CHECK(state.restaurants_for(core::PlayerId{0}).size() ==
          (scenario->start == core::content::ScenarioStart::QuickStart ? 1u
                                                                       : 0u));
    content_test::close_dialogs(state);
    content_test::play_day(state);
    CHECK(state.player(core::PlayerId{0})->status() == PlayerStatus::Active);

    test_support::TempDir dir;
    const fs::path file = dir.path() / "preset.plsave";
    std::string error;
    REQUIRE(SaveGameManager::save(state, game.context, file.string(), &error));
    REQUIRE(SaveGameManager::get_save_version(file.string()) ==
            core::content::expanded_save_version);
    const std::string saved = fingerprint(game);

    content_test::TestGame fresh;
    engine::random::seed(0xBEEF);
    REQUIRE(SaveGameManager::load(fresh.state, fresh.context, file.string(),
                                  &error));
    CHECK(fingerprint(fresh) == saved);
    REQUIRE(fresh.state.scenario() != nullptr);
    CHECK(fresh.state.scenario()->id == scenario->id);
    CHECK(fresh.state.roster() == state.roster());
  }
}
