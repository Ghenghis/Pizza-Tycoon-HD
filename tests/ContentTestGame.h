/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Helpers for the content tests that play a game without views: the shipped
// village and its Cozy Village scenario, a new game from a scenario, and a
// player who ends each day at once and closes every dialog.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "TestSupport.h"
#include "core/GameTypes.h"
#include "core/common/Ids.h"
#include "core/content/LocationValidator.h"
#include "core/content/Roster.h"
#include "core/content/Scenario.h"
#include "core/debug/ScriptedPlayer.h"
#include "core/init/NewGameConfig.h"
#include "core/state/CompetitionSession.h"
#include "core/state/EndOfDayCause.h"
#include "core/state/GameOptions.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "core/time/GameDate.h"
#include "core/util/Constants.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/BobManager.h"
#include "resources/util/FileUtils.h"

#define CONTENT_REQUIRE_GAME_DATA()                                            \
  do {                                                                         \
    const auto content_game_path = test_support::game_path();                  \
    if (!content_game_path) {                                                  \
      SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");            \
    }                                                                          \
    fileutils::game_data_path(content_game_path->string());                    \
  } while (false)

namespace content_test {

namespace fs = std::filesystem;

inline fs::path data_dir() {
  return fs::path(__FILE__).parent_path().parent_path() / "data";
}

inline std::shared_ptr<const core::content::LocationDefinition> load_village() {
  const auto report = core::content::load_location(
      data_dir() / "locations" / "north-pole" / "santas-pizza-village.json");
  INFO(core::content::describe(report.findings));
  REQUIRE(report.ok());
  return report.definition;
}

// One of the shipped presets, data/scenarios/<file>.json.
inline std::shared_ptr<const core::content::Scenario>
load_preset(std::string_view file) {
  const auto read = core::content::read_scenario_file(
      data_dir() / "scenarios" / (std::string(file) + ".json"));
  INFO(file << "\n" << core::content::describe(read.findings));
  REQUIRE(read.ok());
  return read.scenario;
}

inline std::shared_ptr<const core::content::Scenario> load_cozy_village() {
  return load_preset("north-pole-cozy-village");
}

// A game run without views. The quick start loads standard pizzas, which
// needs the sprite sizes, so the context gets the sprite manager.
struct TestGame {
  BobManager bobs{BobLoader{}};
  GameContext context;
  GameState state;

  TestGame() { context.bob = &bobs; }
  TestGame(const TestGame&) = delete;
  TestGame& operator=(const TestGame&) = delete;
};

inline void add_players(init::Config& config) {
  config.players.resize(NUMBER_OF_PLAYERS);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    config.players[i].name = "Player " + std::to_string(i + 1);
    config.players[i].character_id = i;
    config.players[i].is_human = (i == 0);
  }
}

// A new game of the scenario in the village, as the scenario menu would
// start it: player 1 is the only person.
inline void start_village_game(
    TestGame& game, std::uint32_t seed,
    const std::shared_ptr<const core::content::Scenario>& scenario,
    const std::shared_ptr<const core::content::LocationDefinition>& village) {
  engine::random::seed(seed);
  auto& state = game.state;
  state.options().autosave_frequency = AutosaveFrequency::Off;
  state.begin_initialization();
  auto& config = state.initializer();
  core::content::Findings findings;
  const auto roster = core::content::scenario_roster(
      *scenario,
      [&](const core::content::ContentKey& key)
          -> std::shared_ptr<const core::content::LocationDefinition> {
        return key == village->key ? village : nullptr;
      },
      findings);
  INFO(core::content::describe(findings));
  REQUIRE(roster.has_value());
  REQUIRE(core::content::configure_new_game(scenario, *roster, config,
                                            state.options().rules, findings));
  add_players(config);
  state.finalize_initialization(game.context);
  REQUIRE(state.active_player() != nullptr);
}

inline void start_classic_game(TestGame& game, std::uint32_t seed,
                               GameMode mode, int city) {
  engine::random::seed(seed);
  auto& state = game.state;
  state.options().autosave_frequency = AutosaveFrequency::Off;
  state.begin_initialization();
  auto& config = state.initializer();
  config.mode = mode;
  config.size = mode == GameMode::QUICKSTART ? init::RestaurantSize::BIG
                                             : init::RestaurantSize::NONE;
  config.difficulty = init::DifficultyLevel::NORMAL;
  config.city_id = core::CityId{city};
  add_players(config);
  state.finalize_initialization(game.context);
  REQUIRE(state.active_player() != nullptr);
}

// A person who closes every dialog at once, the same way on every run: what
// is above the city view is popped and told it was closed, and the year's
// pizza competition gets no entry. Calls seen(text) for each Maestro message.
// The console's `game dismiss` and `game day` are the same player
// (core/debug/ScriptedPlayer.h).
inline void
close_dialogs(GameState& state,
              const std::function<void(const std::string&)>& seen = nullptr) {
  debug::scripted::close_dialogs(state, seen);
}

inline bool human_turn(const GameState& state) {
  return debug::scripted::human_turn(state);
}

// Ends the human player's day at once and runs the simulation until their
// next day has begun.
inline void
play_day(GameState& state,
         const std::function<void(const std::string&)>& seen = nullptr) {
  const GameDate start = state.time().current_date();
  if (!debug::scripted::play_day(state, seen)) {
    FAIL("The day " << start.to_string() << " did not end within "
                    << debug::scripted::max_ticks_per_day << " ticks");
  }
}

inline void
play_days(GameState& state, int days,
          const std::function<void(const std::string&)>& seen = nullptr) {
  for (int i = 0; i < days; ++i) {
    play_day(state, seen);
  }
}

} // namespace content_test
