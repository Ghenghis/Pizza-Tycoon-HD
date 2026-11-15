/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The World Builder journey, headless (WORLD-BUILDER-EXPANSION.md): a game in
// Santa's Pizza Village adds a North Pole area with the console's `world
// add-area`, goes there with `nav area`, rents and furnishes a pizzeria (the
// slice script's steps, data/review/north-pole-slice.exec), trades a day,
// saves, and loads the save in a fresh GameState with a fresh area catalog.
// The village is never disturbed: its money, businesses and object ids are
// the same before and after the expansion, the save and the travel back and
// forth. Needs the original game's files (PLEGACY_TEST_GAME_PATH).

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "ContentTestGame.h"
#include "TestSupport.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/content/AreaCatalog.h"
#include "core/content/WorldMap.h"
#include "core/debug/command/CommandRegistry.h"
#include "core/debug/command/CommandResult.h"
#include "core/debug/commands/CityCommands.h"
#include "core/debug/commands/GameCommands.h"
#include "core/debug/commands/NavCommands.h"
#include "core/debug/commands/RestaurantCommands.h"
#include "core/debug/commands/StaffCommands.h"
#include "core/debug/commands/WorldCommands.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"
#include "core/restaurant/RestaurantStats.h"
#include "core/save/SaveGameManager.h"

namespace fs = std::filesystem;
using core::content::area_catalog;
using save::SaveGameManager;

namespace {

constexpr std::uint32_t seed = 0x5A7A;
const core::CityId village_city{0};
const std::string village_key = "pthd:santas-pizza-village";

debug::command::CommandRegistry& console() {
  static debug::command::CommandRegistry registry = [] {
    debug::command::CommandRegistry r;
    r.register_command(debug::commands::make_city_commands());
    r.register_command(debug::commands::make_restaurant_commands());
    r.register_command(debug::commands::make_staff_commands());
    r.register_command(debug::commands::make_game_commands());
    r.register_command(debug::commands::make_nav_commands());
    r.register_command(debug::commands::make_world_commands());
    return r;
  }();
  return registry;
}

debug::command::CommandResult try_run(content_test::TestGame& game,
                                      const std::string& line) {
  return console().execute(line, game.state, game.context);
}

std::string run(content_test::TestGame& game, const std::string& line) {
  INFO("console: " << line);
  const auto result = try_run(game, line);
  INFO((result.error_message ? *result.error_message : std::string{}));
  REQUIRE(result.status == debug::command::CommandStatus::Success);
  std::string text;
  for (const auto& m : result.messages) {
    text += m + "\n";
  }
  return text;
}

std::string refused(content_test::TestGame& game, const std::string& line) {
  INFO("console: " << line);
  const auto result = try_run(game, line);
  CHECK(result.status != debug::command::CommandStatus::Success);
  return result.error_message.value_or("") + "\n" +
         result.error_suggestion.value_or("");
}

// What a place holds that must not change unless the person changes it:
// every player's money, and every pizzeria of the city with its owner, its
// premise and the ids of its furniture.
using Pizzeria = std::tuple<int, int, int, std::vector<int>, std::vector<int>,
                            int, long long>;
struct Snapshot {
  std::vector<long long> cash;
  std::vector<Pizzeria> pizzerias;
  std::vector<std::pair<int, int>> owners; // premise index, owner (-1 none)
  friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshot(content_test::TestGame& game, core::CityId city_id) {
  auto& state = game.state;
  Snapshot s;
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    const Player& p = *state.player(core::PlayerId{i});
    s.cash.push_back(p.cash_balance().cents);
    for (const Restaurant* r : state.restaurants_for(p.id())) {
      if (r->property().city().id() != city_id) {
        continue;
      }
      std::vector<int> room;
      std::vector<int> kitchen;
      if (r->layout()) {
        for (const auto& f : r->layout()->placed_furniture()) {
          room.push_back(f->id().value);
        }
        for (const auto& f : r->layout()->placed_kitchen_furniture()) {
          kitchen.push_back(f->id().value);
        }
      }
      s.pizzerias.emplace_back(r->id().value, i, r->property().index(), room,
                               kitchen, r->stats().total_pizzas_sold,
                               r->stats().total_revenue.cents);
    }
  }
  const City* city = state.city(city_id);
  REQUIRE(city);
  for (const Property* p : city->business_premises()) {
    const auto owner = p->owner_or_tenant();
    s.owners.emplace_back(p->index(), owner ? owner->value : -1);
  }
  return s;
}

// The slice script's shop (data/review/north-pole-slice.exec), in whatever
// city is active: the cheapest premise, two tables with chairs, two ovens,
// the menu, stock, a cook and a waiter.
Restaurant& set_up_slice_shop(content_test::TestGame& game) {
  run(game, "city rent cheapest");
  for (const char* line :
       {"restaurant place ROUND_GRILL 46 4",
        "restaurant place TRADITIO_ROMANO 14 4",
        "restaurant place TRADITIO_ROMANO 106 4",
        "restaurant place ROUND_GRILL 46 84",
        "restaurant place TRADITIO_ROMANO 14 84",
        "restaurant place TRADITIO_ROMANO 106 84",
        "restaurant place OVEN_SMALL 0 0", "restaurant place OVEN_SMALL 120 0",
        "restaurant menu 8", "restaurant stock fill 10", "staff hire 0 @ @ 20",
        "staff hire 1 @ @ 21", "restaurant open"}) {
    run(game, line);
  }
  Restaurant* r = game.state.active_restaurant();
  REQUIRE(r);
  return *r;
}

void start(content_test::TestGame& game) {
  area_catalog().clear();
  content_test::start_village_game(game, seed,
                                   content_test::load_cozy_village(),
                                   content_test::load_village());
  content_test::close_dialogs(game.state);
}

std::optional<core::CityId> roster_city(const GameState& state,
                                        const std::string& key) {
  const auto& entries = state.roster().entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].key.str() == key) {
      return core::CityId{static_cast<int>(i)};
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("World Builder: a new North Pole area joins a village game and "
          "survives a save",
          "[content][world-builder][game][save]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  start(game);
  auto& state = game.state;
  const Player& me = *state.player(core::PlayerId{0});
  REQUIRE(state.roster().size() == 1);
  const auto* scenario = state.scenario();
  REQUIRE(scenario);
  const std::string scenario_sha = scenario->sha256;
  const auto village_entry = state.roster().entries()[0];
  const Snapshot village_before = snapshot(game, village_city);

  // Add Area: generated, checked, admitted, listed after the village. The
  // game is not changed by it.
  const std::string added =
      run(game, "world add-area north-pole \"Frostfall Hollow\" 7");
  CHECK(added.find("area-000001") != std::string::npos);
  const core::content::Area* area = area_catalog().find("area-000001");
  REQUIRE(area);
  REQUIRE(area->state == core::content::AreaState::Admitted);
  REQUIRE(area->definition);
  CHECK_FALSE(core::content::has_errors(
      core::content::validate_location(*area->definition)));
  // Copies: a catalog entry is not kept across the catalog's changes.
  const core::content::ContentKey area_content_key = area->key;
  const std::string area_key = area->key.str();
  const std::string area_sha = area->definition->sha256;
  CHECK(state.roster().size() == 1);
  const std::string listed = run(game, "world areas north-pole");
  CHECK(listed.find("Santa's Pizza Village") < listed.find("Frostfall Hollow"));

  // nav area: the game's explicit expansion. The new place is appended; the
  // village keeps its id, its pinned definition and everything in it, and the
  // scenario (its roster and goal) is the same one.
  run(game, "nav area area-000001");
  REQUIRE(state.roster().size() == 2);
  const core::CityId area_city{1};
  CHECK(state.roster().entries()[0] == village_entry);
  CHECK(state.roster().entries()[1].key.str() == area_key);
  CHECK(state.active_city_id() == area_city);
  CHECK(state.city_display_name(area_city) == "Frostfall Hollow");
  CHECK(state.scenario() == scenario);
  CHECK(scenario->roster.size() == 1);
  CHECK(snapshot(game, village_city) == village_before);
  // The world map lists it on the North Pole route.
  bool on_map = false;
  for (const auto& e : core::content::roster_world_entries(state.roster())) {
    if (e.key.str() == area_key) {
      on_map = e.city == area_city && e.bonus_route == "north-pole";
    }
  }
  CHECK(on_map);

  // The slice's shop, in the new area, and a day of trade.
  Restaurant& shop = set_up_slice_shop(game);
  CHECK(shop.property().city().id() == area_city);
  CHECK(shop.property().owner_or_tenant() == me.id());
  content_test::play_day(state);
  CHECK(shop.stats().days_in_operation() >= 1);
  CHECK(me.status() == PlayerStatus::Active);

  const Snapshot village_saved = snapshot(game, village_city);
  const Snapshot area_saved = snapshot(game, area_city);
  REQUIRE_FALSE(area_saved.pizzerias.empty());
  const auto saved_fingerprint =
      SaveGameManager::fingerprint(state, game.context).hash;

  test_support::TempDir dir;
  const fs::path file = dir.path() / "world-builder.plsave";
  std::string error;
  REQUIRE(SaveGameManager::save(state, game.context, file.string(), &error));

  // A fresh game state, and a fresh catalog: what a new process has.
  area_catalog().clear();
  content_test::TestGame fresh;
  fresh.state.options().autosave_frequency = AutosaveFrequency::Off;
  INFO(error);
  REQUIRE(
      SaveGameManager::load(fresh.state, fresh.context, file.string(), &error));
  CHECK(SaveGameManager::fingerprint(fresh.state, fresh.context).hash ==
        saved_fingerprint);
  REQUIRE(fresh.state.roster().size() == 2);
  CHECK(fresh.state.roster().entries()[0] == village_entry);
  REQUIRE(fresh.state.roster().entries()[1].definition);
  CHECK(fresh.state.roster().entries()[1].key.str() == area_key);
  CHECK(fresh.state.roster().entries()[1].definition->sha256 == area_sha);
  REQUIRE(fresh.state.scenario());
  CHECK(fresh.state.scenario()->sha256 == scenario_sha);
  CHECK(snapshot(fresh, village_city) == village_saved);
  CHECK(snapshot(fresh, area_city) == area_saved);

  // Back to the village and forward again, by key and by the id the fresh
  // catalog gave the area it found in the game.
  run(fresh, "nav area " + village_key);
  CHECK(fresh.state.active_city_id() == village_city);
  CHECK(snapshot(fresh, village_city) == village_saved);
  const std::string areas = run(fresh, "world areas north-pole");
  CHECK(areas.find(area_key) != std::string::npos);
  const auto* adopted = area_catalog().find_by_key(area_content_key);
  REQUIRE(adopted);
  run(fresh, "nav area " + adopted->id);
  CHECK(fresh.state.active_city_id() == area_city);
  CHECK(fresh.state.roster().size() == 2);
  CHECK(snapshot(fresh, village_city) == village_saved);
  CHECK(snapshot(fresh, area_city) == area_saved);
  content_test::play_day(fresh.state);
  CHECK(fresh.state.player(core::PlayerId{0})->status() ==
        PlayerStatus::Active);
}

TEST_CASE("World Builder: a second area, a rename, an empty draft and a "
          "failed generation",
          "[content][world-builder][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  start(game);
  auto& state = game.state;
  const Snapshot village_before = snapshot(game, village_city);

  run(game, "world add-area north-pole \"Frostfall Hollow\" 7");
  run(game, "world add-area north-pole \"Glacier Gate\" 1224");
  const auto* first = area_catalog().find("area-000001");
  const auto* second = area_catalog().find("area-000002");
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->key != second->key);
  const auto first_key = first->key;
  const auto second_key = second->key;
  const auto pages = area_catalog().pages("north-pole");
  REQUIRE(pages.size() == 3);
  CHECK(pages[1].area_id == "area-000001");
  CHECK(pages[2].area_id == "area-000002");

  // Both join the game, in the order the person goes to them.
  run(game, "nav area area-000002");
  run(game, "nav area area-000001");
  REQUIRE(state.roster().size() == 3);
  CHECK(state.roster().entries()[1].key == second_key);
  CHECK(state.roster().entries()[2].key == first_key);
  CHECK(snapshot(game, village_city) == village_before);
  // Going again adds nothing.
  run(game, "nav area area-000002");
  CHECK(state.roster().size() == 3);

  // Rename: the same id and key; the catalog has the next revision, the
  // game keeps the one it pinned.
  const int pinned = state.roster().entries()[2].definition->revision;
  run(game, "world area-rename area-000001 \"Frostfall Harbour\"");
  const auto* renamed = area_catalog().find("area-000001");
  REQUIRE(renamed);
  CHECK(renamed->key == first_key);
  CHECK(renamed->name == "Frostfall Harbour");
  CHECK(renamed->definition->revision == pinned + 1);
  CHECK(state.roster().entries()[2].definition->revision == pinned);
  CHECK(state.roster().entries()[2].definition->name == "Frostfall Hollow");
  CHECK(run(game, "world areas north-pole").find("Frostfall Harbour") !=
        std::string::npos);
  refused(game, "world area-rename area-000099 Nowhere");

  // An empty draft: listed, refused for Play with what it needs.
  const std::string draft =
      run(game, "world add-area north-pole \"Blank Slate\" empty");
  CHECK(draft.find("Before Play it needs") != std::string::npos);
  const std::string why = refused(game, "nav area area-000003");
  CHECK(why.find("draft") != std::string::npos);
  CHECK(why.find("\n  - ") != std::string::npos);
  CHECK(state.roster().size() == 3);

  // A failed generation changes nothing: not the catalog, not the game.
  const auto before = area_catalog().pages("north-pole").size();
  refused(game, "world add-area north-pole \"\"");
  refused(game, "world add-area north-pole \"A name far too long for it\"");
  refused(game, "world add-area atlantis \"Deep Town\"");
  refused(game, "world add-area north-pole Snowfield sometimes");
  CHECK(area_catalog().pages("north-pole").size() == before);
  CHECK(state.roster().size() == 3);
  CHECK(snapshot(game, village_city) == village_before);
  CHECK(state.active_city_id() == core::CityId{2});
}
