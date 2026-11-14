/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Content keys, the Classic adapter and the per-game roster
// (docs/studio/location-contract.md sections 2.1-2.3). All but the last test
// run without the original game.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "core/city/CityNames.h"
#include "core/common/Ids.h"
#include "core/content/ClassicCities.h"
#include "core/content/ContentKey.h"
#include "core/content/Roster.h"
#include "core/init/NewGameConfig.h"
#include "core/state/GameState.h"
#include "core/util/Constants.h"
#include "core/util/GameContext.h"
#include "resources/util/FileUtils.h"

using core::CityId;
using core::content::ContentKey;
using core::content::Roster;
using core::content::RosterEntry;
using core::content::RosterSource;

namespace {

ContentKey key(std::string_view text) {
  auto parsed = ContentKey::parse(text);
  REQUIRE(parsed.has_value());
  return *parsed;
}

RosterEntry classic_entry(int number) {
  return RosterEntry{.key = core::content::classic_key(CityId{number}),
                     .source = RosterSource::Classic,
                     .classic_number = number};
}

RosterEntry definition_entry(std::string_view text) {
  return RosterEntry{.key = key(text),
                     .source = RosterSource::Definition,
                     .classic_number = std::nullopt};
}

// The keys the contract gives the original ten, in city id order.
constexpr std::array<std::string_view, NUMBER_OF_CITIES> expected_keys = {
    "classic:paris",  "classic:berlin",  "classic:baltimore",
    "classic:vienna", "classic:chicago", "classic:new-york",
    "classic:madrid", "classic:rome",    "classic:stockholm",
    "classic:london",
};

} // namespace

TEST_CASE("A content key is a namespace and a slug", "[content]") {
  for (std::string_view text :
       {"classic:paris", "pthd:santas-pizza-village", "u-3f9a1c2e:harbor-bay",
        "qa-synthetic:d35-r1-0001", "pthd:7-eleven", "ab:cd"}) {
    INFO(text);
    CHECK(ContentKey::is_valid(text));
    const auto parsed = ContentKey::parse(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->str() == text);
  }

  const ContentKey village = key("pthd:santas-pizza-village");
  CHECK(village.namespace_part() == "pthd");
  CHECK(village.slug() == "santas-pizza-village");
  CHECK(village.in_namespace(core::content::first_party_namespace));
  CHECK_FALSE(village.in_namespace(core::content::classic_namespace));
}

TEST_CASE("Malformed content keys are turned away", "[content]") {
  const std::string long_namespace(33, 'a');
  const std::string long_slug(65, 'b');
  const std::string longest_namespace(32, 'a');
  const std::string longest_slug(64, 'b');
  CHECK(ContentKey::is_valid(longest_namespace + ":" + longest_slug));

  for (const std::string& text : std::vector<std::string>{
           "",
           "paris",
           ":paris",
           "classic:",
           "c:paris",          // namespace too short
           "classic:p",        // slug too short
           "Classic:paris",    // upper case
           "classic:Paris",    //
           "classic:new york", // space
           "classic::paris",   // second colon
           "classic:paris:2",  //
           "1classic:paris",   // namespace must start with a letter
           "-classic:paris",   //
           "classic:-paris",   // slug must start with a letter or digit
           "classic:par_is",   // only a-z, 0-9 and '-'
           "classic:parís",    // not ASCII
           long_namespace + ":paris",
           "classic:" + long_slug,
       }) {
    INFO(text);
    CHECK_FALSE(ContentKey::is_valid(text));
    CHECK_FALSE(ContentKey::parse(text).has_value());
  }
}

TEST_CASE("Content keys compare and hash by their text", "[content]") {
  CHECK(key("classic:paris") == key("classic:paris"));
  CHECK(key("classic:paris") != key("classic:rome"));
  CHECK(key("classic:paris") < key("classic:rome"));

  std::unordered_set<ContentKey> keys{key("classic:paris"),
                                      key("classic:paris"),
                                      key("pthd:santas-pizza-village")};
  CHECK(keys.size() == 2);
}

TEST_CASE("The Classic adapter maps the ten cities both ways",
          "[content][classic]") {
  for (int i = 0; i < NUMBER_OF_CITIES; ++i) {
    INFO("city " << i);
    const ContentKey k = core::content::classic_key(CityId{i});
    CHECK(k.str() == expected_keys[static_cast<std::size_t>(i)]);
    CHECK(k.in_namespace(core::content::classic_namespace));
    const auto back = core::content::classic_city_id(k);
    REQUIRE(back.has_value());
    CHECK(back->value == i);
    // The table and the city names can't drift apart: the names are what
    // the game shows, and a key never changes.
    CHECK(core::content::classic_cities[static_cast<std::size_t>(i)].number ==
          i);
    CHECK(core::city_name(CityId{i}) ==
          core::city_names[static_cast<std::size_t>(i)]);
  }
}

TEST_CASE("The Classic adapter knows no other city", "[content][classic]") {
  for (std::string_view text : {"classic:atlantis", "pthd:santas-pizza-village",
                                "pthd:paris", "u-3f9a1c2e:paris"}) {
    INFO(text);
    CHECK_FALSE(core::content::classic_city_id(key(text)).has_value());
  }
}

TEST_CASE("The Classic roster is the original ten in their order",
          "[content][roster][classic]") {
  const Roster roster = Roster::classic();
  REQUIRE(roster.size() == static_cast<std::size_t>(NUMBER_OF_CITIES));
  CHECK(roster.is_classic());
  for (int i = 0; i < NUMBER_OF_CITIES; ++i) {
    INFO("city " << i);
    REQUIRE(roster.contains(CityId{i}));
    const RosterEntry& entry = roster.entry(CityId{i});
    CHECK(entry.key.str() == expected_keys[static_cast<std::size_t>(i)]);
    CHECK(entry.source == RosterSource::Classic);
    CHECK(entry.classic_number == i);
    const auto found = roster.find(entry.key);
    REQUIRE(found.has_value());
    CHECK(found->value == i);
  }
  CHECK_FALSE(roster.contains(CityId{-1}));
  CHECK_FALSE(roster.contains(CityId{NUMBER_OF_CITIES}));
  CHECK_FALSE(roster.find(key("pthd:santas-pizza-village")).has_value());
  CHECK(Roster::classic() == roster);
}

TEST_CASE("A roster follows its rules", "[content][roster]") {
  SECTION("Classic and new places can share a roster") {
    const auto mixed = Roster::from_entries(
        {classic_entry(0), definition_entry("pthd:santas-pizza-village")});
    REQUIRE(mixed.has_value());
    CHECK(mixed->size() == 2);
    CHECK_FALSE(mixed->is_classic());
    CHECK(mixed->find(key("pthd:santas-pizza-village"))->value == 1);
    CHECK(mixed->find(key("classic:paris"))->value == 0);
  }

  SECTION("The Classic ten built from entries are the Classic roster") {
    std::vector<RosterEntry> entries;
    for (int i = 0; i < NUMBER_OF_CITIES; ++i) {
      entries.push_back(classic_entry(i));
    }
    const auto rebuilt = Roster::from_entries(std::move(entries));
    REQUIRE(rebuilt.has_value());
    CHECK(rebuilt->is_classic());
    CHECK(*rebuilt == Roster::classic());
  }

  SECTION("Order is part of the roster") {
    const auto swapped = Roster::from_entries(
        {classic_entry(1), classic_entry(0), classic_entry(2), classic_entry(3),
         classic_entry(4), classic_entry(5), classic_entry(6), classic_entry(7),
         classic_entry(8), classic_entry(9)});
    REQUIRE(swapped.has_value());
    CHECK_FALSE(swapped->is_classic());
  }

  SECTION("Empty and oversized rosters are turned away") {
    CHECK_FALSE(Roster::from_entries({}).has_value());
    std::vector<RosterEntry> too_many;
    for (std::size_t i = 0; i <= Roster::max_size; ++i) {
      too_many.push_back(
          definition_entry("pthd:place-" + std::to_string(i + 10)));
    }
    CHECK_FALSE(Roster::from_entries(std::move(too_many)).has_value());
  }

  SECTION("A place appears once") {
    CHECK_FALSE(
        Roster::from_entries({classic_entry(3), classic_entry(3)}).has_value());
    CHECK_FALSE(Roster::from_entries({definition_entry("pthd:harbor-bay"),
                                      definition_entry("pthd:harbor-bay")})
                    .has_value());
  }

  SECTION("Classic entries carry the right key and number") {
    RosterEntry wrong_number = classic_entry(2);
    wrong_number.classic_number = 3;
    CHECK_FALSE(Roster::from_entries({wrong_number}).has_value());

    RosterEntry no_number = classic_entry(2);
    no_number.classic_number = std::nullopt;
    CHECK_FALSE(Roster::from_entries({no_number}).has_value());

    RosterEntry not_classic = definition_entry("pthd:paris");
    not_classic.source = RosterSource::Classic;
    not_classic.classic_number = 0;
    CHECK_FALSE(Roster::from_entries({not_classic}).has_value());
  }

  SECTION("Other entries stay out of the classic and QA namespaces") {
    CHECK_FALSE(Roster::from_entries({definition_entry("classic:atlantis")})
                    .has_value());
    CHECK_FALSE(
        Roster::from_entries({definition_entry("qa-synthetic:d35-r1-0001")})
            .has_value());
    RosterEntry numbered = definition_entry("pthd:santas-pizza-village");
    numbered.classic_number = 0;
    CHECK_FALSE(Roster::from_entries({numbered}).has_value());
  }
}

TEST_CASE("A game state and a new game start with the Classic roster",
          "[content][roster][classic]") {
  const GameState state;
  CHECK(state.roster().is_classic());
  CHECK(state.roster() == Roster::classic());

  const init::Config config;
  CHECK(config.roster.is_classic());
}

TEST_CASE("A started Classic game keeps the Classic roster",
          "[content][roster][classic][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  GameContext context;
  GameState state;
  state.options().autosave_frequency = AutosaveFrequency::Off;
  state.begin_initialization();
  auto& config = state.initializer();
  CHECK(config.roster.is_classic());
  config.mode = GameMode::FREEMODE;
  config.difficulty = init::DifficultyLevel::NORMAL;
  config.city_id = CityId{3};
  config.players.resize(NUMBER_OF_PLAYERS);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    config.players[i].name = "Player " + std::to_string(i + 1);
    config.players[i].character_id = i;
    config.players[i].is_human = (i == 0);
  }
  state.finalize_initialization(context);

  REQUIRE(state.roster().is_classic());
  const CityId vienna = state.active_city_id();
  CHECK(vienna.value == 3);
  CHECK(state.roster().entry(vienna).key.str() == "classic:vienna");
  CHECK(state.active_city()->name() ==
        core::city_name(vienna)); // names still come from CityNames.h
}
