/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The scenario goal observer (location-contract.md section 11): objectives
// read from the career log, the victory message of a sandbox scenario that
// leaves the player in the game, the Classic win that still ends it, and the
// city-numbered milestones that belong to the original ten only.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ContentTestGame.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/content/Scenario.h"
#include "core/content/ScenarioGoals.h"
#include "core/events/CareerLog.h"
#include "core/events/MilestoneId.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "core/time/GameDate.h"

using core::content::AfterGoal;
using core::content::evaluate_goals;
using core::content::Objective;
using core::content::Polarity;
using core::content::Scenario;
using core::events::CareerLog;
using core::events::MilestoneId;

namespace {

GameDate day(int n) { return game_start_date.add_days(n); }

CareerLog::CvEntry entry(MilestoneId id, int on_day) {
  return CareerLog::CvEntry{
      .date = day(on_day), .message_id = id, .shown = true};
}

Objective objective(MilestoneId id, Polarity polarity,
                    std::optional<int> within = std::nullopt) {
  return Objective{.id = "o",
                   .milestone = id,
                   .polarity = polarity,
                   .label = "objective",
                   .within_game_days = within};
}

bool recorded(const GameState& state, core::PlayerId pid, MilestoneId id) {
  const auto cv = state.career_log().get_cv_entries_for(pid);
  return std::any_of(cv.begin(), cv.end(), [id](const CareerLog::CvEntry& e) {
    return e.message_id == id;
  });
}

bool any_arrival(const GameState& state, core::PlayerId pid) {
  for (int i = 0; i < NUMBER_OF_CITIES; ++i) {
    if (recorded(state, pid,
                 static_cast<MilestoneId>(
                     static_cast<int>(MilestoneId::ArrivalInParis) + i))) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("The goal observer reads the Cozy Village objectives",
          "[content][goals]") {
  const auto scenario = content_test::load_cozy_village();

  auto progress = evaluate_goals(*scenario, {}, day(0));
  CHECK_FALSE(progress.reached_on.has_value());
  CHECK_FALSE(progress.failed);
  REQUIRE(progress.optional_objectives.size() == 2);
  CHECK_FALSE(progress.optional_objectives[0].met); // fast service
  CHECK(progress.optional_objectives[1].met);       // no loan yet

  std::vector<CareerLog::CvEntry> cv = {
      entry(MilestoneId::OpenedFirstRestaurant, 1),
      entry(MilestoneId::HasAssetsOf100K, 10)};
  progress = evaluate_goals(*scenario, cv, day(12));
  CHECK_FALSE(progress.reached_on.has_value());
  CHECK(progress.goal[0].met);
  CHECK_FALSE(progress.goal[1].met);

  cv.push_back(entry(MilestoneId::TakesOutFirstLoan, 20));
  cv.push_back(entry(MilestoneId::AwardedFirstStar, 40));
  progress = evaluate_goals(*scenario, cv, day(41));
  REQUIRE(progress.reached_on.has_value());
  CHECK(*progress.reached_on == day(40));
  // The loan breaks the optional challenge, not the goal.
  CHECK_FALSE(progress.optional_objectives[1].met);
  CHECK(progress.optional_objectives[1].recorded_on == day(20));

  CHECK(core::content::completes_goal(*scenario,
                                      {MilestoneId::AwardedFirstStar}));
  CHECK_FALSE(core::content::completes_goal(*scenario,
                                            {MilestoneId::TakesOutFirstLoan}));
  CHECK_FALSE(core::content::completes_goal(*scenario, {}));
}

TEST_CASE("An avoid objective in the goal must hold until the goal is met",
          "[content][goals]") {
  Scenario s;
  s.goal = {objective(MilestoneId::HasAssetsOf100K, Polarity::Achieve),
            objective(MilestoneId::TakesOutFirstLoan, Polarity::Avoid)};

  const auto broken_first =
      evaluate_goals(s,
                     {entry(MilestoneId::TakesOutFirstLoan, 5),
                      entry(MilestoneId::HasAssetsOf100K, 10)},
                     day(11));
  CHECK_FALSE(broken_first.reached_on.has_value());
  CHECK(broken_first.failed);

  const auto broken_after =
      evaluate_goals(s,
                     {entry(MilestoneId::HasAssetsOf100K, 10),
                      entry(MilestoneId::TakesOutFirstLoan, 12)},
                     day(13));
  REQUIRE(broken_after.reached_on.has_value());
  CHECK(*broken_after.reached_on == day(10));
  CHECK_FALSE(broken_after.failed);

  // A goal of avoid objectives alone is never "reached".
  Scenario only_avoid;
  only_avoid.goal = {
      objective(MilestoneId::TakesOutFirstLoan, Polarity::Avoid)};
  CHECK_FALSE(evaluate_goals(only_avoid, {}, day(100)).reached_on.has_value());
}

TEST_CASE("Objective windows count from the first day of the game",
          "[content][goals]") {
  Scenario s;
  s.goal = {objective(MilestoneId::Sold1KPizzasPerDay, Polarity::Achieve, 30)};

  auto progress =
      evaluate_goals(s, {entry(MilestoneId::Sold1KPizzasPerDay, 30)}, day(30));
  CHECK(progress.reached_on == day(30));

  progress =
      evaluate_goals(s, {entry(MilestoneId::Sold1KPizzasPerDay, 31)}, day(31));
  CHECK_FALSE(progress.reached_on.has_value());
  CHECK(progress.failed);

  progress = evaluate_goals(s, {}, day(29));
  CHECK_FALSE(progress.failed);
  progress = evaluate_goals(s, {}, day(31));
  CHECK(progress.failed);
}

TEST_CASE("Cozy Village announces its goal once and plays on",
          "[content][goals][game][village]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const auto scenario = content_test::load_cozy_village();
  content_test::start_village_game(game, 0x5A17A, scenario,
                                   content_test::load_village());
  auto& state = game.state;
  auto* player = state.active_player();
  REQUIRE(player != nullptr);
  REQUIRE(player->is_human_controlled());
  const auto pid = player->id();
  REQUIRE(state.scenario() == scenario.get());

  std::vector<std::string> messages;
  const auto seen = [&](const std::string& text) { messages.push_back(text); };
  content_test::close_dialogs(state, seen);
  messages.clear();

  state.record_milestone(MilestoneId::HasAssetsOf100K, pid);
  state.process_pending_milestones();
  content_test::close_dialogs(state, seen);
  CHECK(messages.empty());

  state.record_milestone(MilestoneId::AwardedFirstStar, pid);
  CHECK(state.process_pending_milestones());
  content_test::close_dialogs(state, seen);
  REQUIRE(messages.size() == 1);
  CHECK(messages[0] == scenario->victory_summary);
  CHECK(player->status() == PlayerStatus::Active);

  // Nothing more to announce, and the game goes on.
  CHECK_FALSE(state.process_pending_milestones());
  content_test::play_days(state, 2, seen);
  CHECK(messages.size() == 1);
  CHECK(player->status() == PlayerStatus::Active);
  CHECK_FALSE(state.ui().is_top<GameOverState>());

  // Even the win sequence (reachable from the console) leaves the player in
  // a sandbox scenario.
  REQUIRE(state.trigger_win_sequence(pid));
  REQUIRE(state.ui().is_top<WinSequenceState>());
  state.ui().pop();
  state.finish_win_sequence(pid);
  CHECK(player->status() == PlayerStatus::Active);
  CHECK_FALSE(state.ui().is_top<GameOverState>());
}

TEST_CASE("A Classic win still ends the game", "[classic][goals][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  content_test::start_classic_game(game, 1994, GameMode::FREEMODE, 0);
  auto& state = game.state;
  CHECK(state.scenario() == nullptr);
  auto* player = state.active_player();
  REQUIRE(player != nullptr);
  content_test::close_dialogs(state);

  REQUIRE(state.trigger_win_sequence(player->id()));
  REQUIRE(state.ui().is_top<WinSequenceState>());
  state.ui().pop();
  state.finish_win_sequence(player->id());
  CHECK(player->status() == PlayerStatus::Eliminated);
  REQUIRE(state.ui().is_top<GameOverState>());
  CHECK(state.ui().top_as<GameOverState>()->cause ==
        GameOverCause::PizzaTycoon);
}

TEST_CASE("City-numbered milestones belong to the original ten",
          "[content][roster][game]") {
  CONTENT_REQUIRE_GAME_DATA();

  SECTION("an original city keeps its arrival milestone") {
    content_test::TestGame game;
    content_test::start_classic_game(game, 7, GameMode::FREEMODE, 3);
    auto& state = game.state;
    auto* player = state.active_player();
    REQUIRE(player != nullptr);
    state.on_city_entered(core::CityId{3}, *player);
    CHECK(recorded(state, player->id(), MilestoneId::ArrivalInVienna));
  }

  SECTION("a new location has none") {
    content_test::TestGame game;
    content_test::start_village_game(game, 7, content_test::load_cozy_village(),
                                     content_test::load_village());
    auto& state = game.state;
    auto* player = state.active_player();
    REQUIRE(player != nullptr);
    state.on_city_entered(core::CityId{0}, *player);
    CHECK_FALSE(any_arrival(state, player->id()));
    CHECK_FALSE(
        recorded(state, player->id(), MilestoneId::DominatesMarketInParis));
  }
}

TEST_CASE("A village game is played in its one location",
          "[content][roster][game][village]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const auto village = content_test::load_village();
  content_test::start_village_game(game, 99, content_test::load_cozy_village(),
                                   village);
  auto& state = game.state;
  CHECK_FALSE(state.roster().is_classic());
  REQUIRE(state.roster().size() == 1);
  CHECK(state.in_roster(core::CityId{0}));
  CHECK_FALSE(state.in_roster(core::CityId{1}));
  CHECK(state.city_display_name(core::CityId{0}) == "Santa's Pizza Village");
  CHECK(state.active_city_id() == core::CityId{0});

  const City* city = state.city(core::CityId{0});
  REQUIRE(city != nullptr);
  CHECK(city->name() == "Santa's Pizza Village");
  CHECK(city->properties().size() == village->properties.size());
  CHECK(city->mayor() != nullptr);
  CHECK(state.initialized_cities().size() == 1);
  CHECK(state.city(core::CityId{1}) == nullptr);

  // Quick start: the person has a furnished restaurant in the village.
  const auto restaurants = state.restaurants_for(core::PlayerId{0});
  REQUIRE(restaurants.size() == 1);
  CHECK(&restaurants[0]->property().city() == city);
}
