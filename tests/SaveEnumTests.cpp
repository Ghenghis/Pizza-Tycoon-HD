/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Every enum a save holds is read with checked_enum: a value the game never
// writes turns the save away, naming the field, instead of being cast into
// an enum that later indexes a table past its end.
//
// The readers run here on their own, on objects built without the original
// game's files. Each bad value throws before anything that would need those
// files.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/CityTypes.h"
#include "core/city/IngredientTrendSystem.h"
#include "core/city/JobMarket.h"
#include "core/city/MayorOffice.h"
#include "core/city/Property.h"
#include "core/food/CustomerTasteProfiles.h"
#include "core/food/IngredientType.h"
#include "core/food/Pizza.h"
#include "core/newspaper/NewspaperEventSystem.h"
#include "core/restaurant/Restaurant.h"
#include "core/save/SaveGameCharacter.h"
#include "core/save/SaveGameCity.h"
#include "core/save/SaveGameEnums.h"
#include "core/save/SaveGameFood.h"
#include "core/save/SaveGameRestaurant.h"

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

// The helper

TEST_CASE("checked_enum takes the values an enum names and nothing else",
          "[save][enum]") {
  for (int value = 0; value <= 5; ++value) {
    CHECK(save::checked_enum<PremiseStatus>(json(value), "a premise status") ==
          static_cast<PremiseStatus>(value));
  }
  for (const json& bad :
       {json(6), json(-1), json(1.5), json("2"), json(true), json(nullptr),
        json(1LL << 40), json::array({1}), json::object()}) {
    INFO("value " << bad.dump());
    CHECK_THROWS_AS(save::checked_enum<PremiseStatus>(bad, "a premise status"),
                    std::out_of_range);
  }

  // The message names the field and the value.
  CHECK_THROWS_WITH(
      save::checked_enum<PremiseStatus>(json(9), "a premise status"),
      ContainsSubstring("a premise status") && ContainsSubstring("9"));
}

TEST_CASE("checked_enum by range leaves out what lies beyond it",
          "[save][enum]") {
  // TrendDuration ends in Count, which is no duration.
  for (int value = 0; value <= 3; ++value) {
    CHECK(save::checked_enum(json(value), TrendDuration::None,
                             TrendDuration::Countdown,
                             "a trend") == static_cast<TrendDuration>(value));
  }
  CHECK_THROWS_AS(save::checked_enum(json(4), TrendDuration::None,
                                     TrendDuration::Countdown, "a trend"),
                  std::out_of_range);
  CHECK_THROWS_AS(save::checked_enum(json(-1), TrendDuration::None,
                                     TrendDuration::Countdown, "a trend"),
                  std::out_of_range);
}

TEST_CASE("checked_enum_or falls back only when the key is missing",
          "[save][enum]") {
  const json missing = json::object();
  CHECK(save::checked_enum_or(missing, "channel", AdvertisingChannel::POSTERS,
                              "a channel") == AdvertisingChannel::POSTERS);
  CHECK(save::checked_enum_or(json{{"channel", 3}}, "channel",
                              AdvertisingChannel::FLYERS,
                              "a channel") == AdvertisingChannel::TV_ADS);
  CHECK_THROWS_AS(save::checked_enum_or(json{{"channel", 4}}, "channel",
                                        AdvertisingChannel::FLYERS,
                                        "a channel"),
                  std::out_of_range);
}

// Players

namespace {

// A player with a hand-made character, as the save tests use.
struct TestPlayer {
  core::character::CharacterData character{};
  Player player{core::PlayerId{0}, core::character::Character(&character),
                true};
};

// The least a player's save needs, plus the fields under test.
json player_state(json fields) {
  json j = {{"name", "Test"}, {"jail_time", 0}};
  j.update(fields);
  return j;
}

void load(TestPlayer& test, const json& j) {
  save::load_player_state(j, test.player,
                          core::food::default_customer_taste_profiles());
}

} // namespace

TEST_CASE("A player's enums load when the game wrote them", "[save][enum]") {
  TestPlayer test;
  load(test,
       player_state(
           {{"awards", {3, 0, 1, 2}},
            {"insurance_state", {{"pending_claim", 4}}},
            {"pending_notifications",
             {{{"text", "hello"}, {"severity", 2}, {"scope", 3}}}},
            {"mafia_state", {{"active_mission", {{"mission_type", 4}}}}}}));
  CHECK(test.player.name() == "Test");
  CHECK(test.player.awards()[0] == AwardLevel::Gold);
  CHECK(test.player.insurance_state().pending_claim == InsuranceType::LIFE);
  REQUIRE(test.player.pending_notifications().size() == 1);
  CHECK(test.player.pending_notifications()[0].severity ==
        StatusSeverity::Error);
  CHECK(test.player.pending_notifications()[0].scope == StatusScope::View);
  REQUIRE(test.player.mafia_state().active_mission.has_value());
  CHECK(test.player.mafia_state().active_mission->mission_type ==
        MafiaMissionType::Investment);
}

TEST_CASE("A player's enums out of range turn the save away", "[save][enum]") {
  for (const json& fields : {
           json{{"awards", {0, 4}}},
           json{{"insurance_state", {{"pending_claim", 5}}}},
           json{{"contracts", {{{"category", 4}, {"remaining_term", 1}}}}},
           json{{"contracts",
                 {{{"category", 0},
                   {"remaining_term", 1},
                   {"items", {{{"type", 22}, {"kg_per_day", 1}}}}}}}},
           json{{"pending_notifications", {{{"severity", 3}}}}},
           json{{"pending_notifications", {{{"scope", -1}}}}},
           json{{"campaigns", {{{"channel", 4}}}}},
           json{{"campaigns", {{{"channel", 2}, {"newspaper_ad_format", 4}}}}},
           json{{"mafia_state", {{"active_mission", {{"mission_type", 5}}}}}},
       }) {
    INFO("fields " << fields.dump());
    TestPlayer test;
    CHECK_THROWS_AS(load(test, player_state(fields)), std::out_of_range);
  }
}

// Cities

TEST_CASE("A city's enums load when the game wrote them", "[save][enum]") {
  City city(core::CityId{0});
  save::json_to_city({{"job_market", {0, 5, 8, 15, 16, 23}},
                      {"ingredient_trends",
                       {{{"slot", 0},
                         {"current", 1},
                         {"target", 2},
                         {"duration", 3},
                         {"approaching", true}}}},
                      {"newspaper_state", {{"target_ingredient", 84}}},
                      // Event 0 is a slot with a countdown and no event yet.
                      {"newspaper_scheduled",
                       {{{"slot", 0}, {"event", 0}, {"countdown", 3}},
                        {{"slot", 1}, {"event", 50}, {"countdown", 1}}}},
                      {"mayor_office", {{"current_quarterly_award", 6}}},
                      {"central_depots", {{{"stock_stolen", 3}}}}},
                     city);

  const auto& slots = city.job_market().slots();
  REQUIRE(slots.size() == 6);
  CHECK(slots[5] == static_cast<core::character::MarketStatus>(23));
  CHECK(city.ingredient_trend_system().durations()[0] ==
        TrendDuration::Countdown);
  CHECK(city.newspaper_state().target_ingredient ==
        core::food::IngredientType::OREGANO);
  CHECK(city.newspaper_event_system().scheduled_types()[1] ==
        core::newspaper::NewspaperEvent::ElderlyPreferSweet);
  CHECK(city.mayor_office().current_quarterly_award() ==
        MayorAwardType::TransAtlanticTrade);
  CHECK(city.central_depot(core::PlayerId{0}).stock_stolen() ==
        IngredientCategory::CHEESE);

  Property property(1, PropertyType::BUSINESS_PREMISE, Size{4, 4},
                    core::ActorId{0}, PremiseStatus::FOR_SALE, city);
  save::json_to_property({{"status", 2}}, property);
  CHECK(property.status() == PremiseStatus::FOR_RENT);
}

TEST_CASE("A city's enums out of range turn the save away", "[save][enum]") {
  for (const json& fields : {
           // 6 and 7 lie between the reserve and the hired statuses.
           json{{"job_market", {0, 6}}},
           json{{"job_market", {24}}},
           json{{"ingredient_trends",
                 {{{"slot", 0},
                   {"current", 0},
                   {"target", 0},
                   {"duration", 4},
                   {"approaching", true}}}}},
           json{{"newspaper_state", {{"target_ingredient", 22}}}},
           json{{"newspaper_scheduled",
                 {{{"slot", 0}, {"event", 51}, {"countdown", 1}}}}},
           json{{"mayor_office", {{"current_quarterly_award", 7}}}},
           json{{"central_depots", {{{"stock_stolen", 4}}}}},
       }) {
    INFO("fields " << fields.dump());
    City city(core::CityId{0});
    CHECK_THROWS_AS(save::json_to_city(fields, city), std::out_of_range);
  }

  City city(core::CityId{0});
  Property property(1, PropertyType::BUSINESS_PREMISE, Size{4, 4},
                    core::ActorId{0}, PremiseStatus::FOR_SALE, city);
  CHECK_THROWS_AS(save::json_to_property({{"status", 6}}, property),
                  std::out_of_range);
  CHECK(property.status() == PremiseStatus::FOR_SALE);
}

// Restaurants

TEST_CASE("A restaurant's enums load when the game wrote them",
          "[save][enum]") {
  const ManagerSettings settings =
      save::json_to_manager_settings({{"turnover_decrease", 2},
                                      {"staff_shortage", 1},
                                      {"behavior_towards_staff", 6}});
  CHECK(settings.turnover_decrease == ManagerReaction::ActYourself);
  CHECK(settings.staff_shortage == ManagerReaction::CallMe);
  CHECK(settings.behavior_towards_staff == ManagerBehavior::Warmly);
}

TEST_CASE("A restaurant's enums out of range turn the save away",
          "[save][enum]") {
  CHECK_THROWS_AS(save::json_to_manager_settings({{"supply_shortage", 3}}),
                  std::out_of_range);
  CHECK_THROWS_AS(
      save::json_to_manager_settings({{"behavior_towards_staff", 7}}),
      std::out_of_range);

  // The job and the course are checked before the character is built.
  CHECK_THROWS_AS(save::json_to_staff({{"job_role", 4}, {"character", {}}}),
                  std::out_of_range);
  CHECK_THROWS_AS(
      save::json_to_staff(
          {{"job_role", 0}, {"course", 18}, {"character", {{"id", 0}}}}),
      std::out_of_range);

  // Checked before the furniture is looked up.
  for (const int type : {-1, 163}) {
    INFO("furniture type " << type);
    CHECK_THROWS_AS(
        save::json_to_placed_furniture({{"furniture_type", type},
                                        {"id", 1},
                                        {"origin", {{"x", 0}, {"y", 0}}},
                                        {"bob_state", 0}}),
        std::out_of_range);
  }
}

// Pizzas

TEST_CASE("A pizza's enums", "[save][enum]") {
  const auto& profiles = core::food::default_customer_taste_profiles();
  const json pizza = {{"name", "Test"}, {"origin", 1}, {"finalized", false}};

  CHECK(save::json_to_pizza(pizza, profiles).origin() ==
        core::food::Origin::User);

  json other_origin = pizza;
  other_origin["origin"] = 2;
  CHECK_THROWS_AS(save::json_to_pizza(other_origin, profiles),
                  std::out_of_range);

  // COUNT ends the list of standard pizzas and is no pizza.
  CHECK_THROWS_AS(save::json_to_recipe({{"id", 1},
                                        {"display_name", "Test"},
                                        {"pizza", pizza},
                                        {"reference_id", 14}},
                                       profiles),
                  std::out_of_range);

  // An ingredient the game doesn't have, here in a gap of IngredientType, is
  // turned away before the topping is built.
  CHECK_THROWS_AS(save::json_to_topping({{"ingredient_id", 22},
                                         {"state", 0},
                                         {"origin", {{"x", 0}, {"y", 0}}}}),
                  std::out_of_range);
  CHECK_THROWS_AS(save::json_to_topping({{"ingredient_id", 96},
                                         {"state", 0},
                                         {"origin", {{"x", 0}, {"y", 0}}}}),
                  std::out_of_range);
}
