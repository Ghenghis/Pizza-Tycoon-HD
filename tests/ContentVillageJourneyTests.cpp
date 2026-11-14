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

#include "ContentJourney.h"
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
using namespace content_journey;

namespace {

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
