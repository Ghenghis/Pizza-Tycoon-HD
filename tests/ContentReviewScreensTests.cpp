/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The Expanded screens a reviewer captures headlessly are reachable from
// console lines, so an --exec script can open each one and `screenshot` it
// (the screenshot command builds the view of whatever is on top of the UI
// stack). These tests run the same lines and check what is on top:
//
//   the North Pole preset chooser:
//     --exec "nav northpole" --exec "screenshot np-presets.png"
//   the world map views, in the review game whose places span three views:
//     --exec "game scenario review-world-tour 7"
//     --exec "nav worldmap europe" --exec "screenshot wm-europe.png"
//     --exec "nav worldmap north-america" --exec "screenshot wm-na.png"
//     --exec "nav worldmap bonus" --exec "screenshot wm-bonus.png"
//   a village restaurant interior:
//     --exec "game scenario north-pole-cozy-village 23"
//     --exec "nav restaurant @" --exec "screenshot village-interior.png"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include "ContentJourney.h"
#include "ContentTestGame.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/common/Ids.h"
#include "core/content/Regions.h"
#include "core/content/Roster.h"
#include "core/content/ShippedScenarios.h"
#include "core/content/WorldMap.h"
#include "core/restaurant/Restaurant.h"
#include "core/state/GameOptions.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"

namespace fs = std::filesystem;
using content_journey::run;
using content_journey::try_run;

namespace {

bool shipped_scenario_exists(std::string_view file) {
  return fs::exists(core::content::shipped_data_dir() / "scenarios" /
                    (std::string(file) + ".json"));
}

#define REQUIRE_SHIPPED_SCENARIO(file)                                         \
  do {                                                                         \
    if (!shipped_scenario_exists(file)) {                                      \
      SKIP("The game's data folder ("                                          \
           << core::content::shipped_data_dir().string()                       \
           << ") has no scenarios; run from the build folder");                \
    }                                                                          \
  } while (false)

} // namespace

TEST_CASE("Review screens: the North Pole preset chooser opens from the "
          "console",
          "[content][review][village][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  run(game, "nav northpole");
  const auto* tag = game.state.ui().top_as<ScreenTag>();
  REQUIRE(tag != nullptr);
  CHECK(tag->kind == ScreenTag::Kind::MenuNorthPole);
  run(game, "nav np");
  REQUIRE(game.state.ui().top_as<ScreenTag>() != nullptr);
  CHECK(game.state.ui().top_as<ScreenTag>()->kind ==
        ScreenTag::Kind::MenuNorthPole);
}

TEST_CASE("Review screens: every world map view opens browse only in the "
          "review game",
          "[content][review][worldmap][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  REQUIRE_SHIPPED_SCENARIO("review-world-tour");
  content_test::TestGame game;
  game.state.options().autosave_frequency = AutosaveFrequency::Off;
  run(game, "game scenario review-world-tour 7");
  auto& state = game.state;
  REQUIRE(state.scenario() != nullptr);
  REQUIRE_FALSE(state.roster().is_classic());
  CHECK(state.roster().size() == 4);
  content_test::close_dialogs(state);

  // Its places fill three views: Paris and Madrid in Europe, New York in
  // North America and the village on the bonus routes.
  core::content::WorldBrowser browser(
      core::content::roster_world_entries(state.roster()), 12);
  const auto europe = core::content::region_index("europe");
  const auto north_america = core::content::region_index("north-america");
  REQUIRE(europe);
  REQUIRE(north_america);
  CHECK(browser.count_in(*europe) == 2);
  CHECK(browser.count_in(*north_america) == 1);
  CHECK(browser.count_in(core::content::WorldBrowser::bonus_view) == 1);

  const auto date = state.time().current_date();
  const auto blocks = state.time().time_blocks();
  const auto cash = state.player(core::PlayerId{0})->cash_balance();
  const auto city = state.active_city_id();

  const auto expect_view = [&](const std::string& line,
                               std::optional<int> view) {
    INFO(line);
    run(game, line);
    const auto* map = state.ui().top_as<EuroMapState>();
    REQUIRE(map != nullptr);
    CHECK(map->browse_only);
    CHECK(map->mode == MapMode::TRAVEL);
    CHECK(map->world_view == view);
    state.ui().pop();
  };
  expect_view("nav worldmap", std::nullopt);
  for (std::size_t i = 0; i < core::content::regions.size(); ++i) {
    expect_view("nav worldmap " + std::string(core::content::regions[i].id),
                static_cast<int>(i));
    expect_view("nav worldmap " + std::to_string(i), static_cast<int>(i));
  }
  expect_view("nav worldmap bonus",
              static_cast<int>(core::content::WorldBrowser::bonus_view));
  expect_view("nav wm 8",
              static_cast<int>(core::content::WorldBrowser::bonus_view));

  CHECK(try_run(game, "nav worldmap atlantis").status !=
        debug::command::CommandStatus::Success);
  CHECK(try_run(game, "nav worldmap 9").status !=
        debug::command::CommandStatus::Success);

  // Opening the views moved nothing.
  CHECK(state.time().current_date() == date);
  CHECK(state.time().time_blocks() == blocks);
  CHECK(state.player(core::PlayerId{0})->cash_balance() == cash);
  CHECK(state.active_city_id() == city);
}

TEST_CASE("Review screens: a Classic game has no world map",
          "[content][review][worldmap][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  content_test::start_classic_game(game, 0x0C1A, GameMode::FREEMODE, 0);
  REQUIRE(game.state.roster().is_classic());
  const std::size_t depth = game.state.ui().size();
  CHECK(try_run(game, "nav worldmap europe").status !=
        debug::command::CommandStatus::Success);
  CHECK(game.state.ui().size() == depth);
}

TEST_CASE("Review screens: a village restaurant interior opens from the "
          "console",
          "[content][review][village][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  REQUIRE_SHIPPED_SCENARIO(core::content::cozy_village.file);
  content_test::TestGame game;
  game.state.options().autosave_frequency = AutosaveFrequency::Off;
  run(game, "game scenario north-pole-cozy-village 23");
  auto& state = game.state;
  content_test::close_dialogs(state);
  const auto mine = state.restaurants_for(core::PlayerId{0});
  REQUIRE(mine.size() == 1);
  const Restaurant& shop = *mine[0];

  run(game, "nav restaurant " + std::to_string(shop.id().value));
  const auto* view = state.ui().top_as<RestaurantViewState>();
  REQUIRE(view != nullptr);
  CHECK(view->id == shop.id());
  CHECK(state.active_restaurant() == &shop);
  CHECK(state.city_display_name(shop.property().city().id()) ==
        "Santa's Pizza Village");
}
