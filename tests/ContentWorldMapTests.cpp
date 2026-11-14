/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The Expanded world map (contract step 9, WORLD-REGIONS-AND-PAGINATION.md):
// list paging against the catalog QA oracles, region assignment by placement,
// deterministic fares, and browsing that leaves the game as it was.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "ContentTestGame.h"
#include "core/content/ClassicCities.h"
#include "core/content/LocationReader.h"
#include "core/content/Roster.h"
#include "core/content/WorldMap.h"
#include "core/save/SaveGameManager.h"
#include "engine/Random.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace core::content;

namespace {

fs::path fixtures() {
  return fs::path(__FILE__).parent_path() / "fixtures" / "locations";
}

json read_json(const fs::path& path) {
  std::ifstream in(path);
  REQUIRE(in.good());
  return json::parse(in);
}

// A catalog QA entry as a world entry (UI-only synthetic data).
std::vector<WorldEntry> catalog_entries(const json& catalog) {
  std::vector<WorldEntry> out;
  for (const auto& e : catalog.at("entries")) {
    WorldEntry w;
    w.key = *ContentKey::parse(e.at("location_key").get<std::string>());
    w.name = e.at("display_name").get<std::string>();
    const auto& placement = e.at("placement");
    if (placement.at("kind") == "region") {
      w.region = region_index(placement.at("region").get<std::string>());
    }
    else {
      w.bonus_route = placement.at("route").get<std::string>();
    }
    out.push_back(std::move(w));
  }
  return out;
}

std::shared_ptr<const LocationDefinition> definition_from(const json& doc) {
  auto read = read_location(doc);
  INFO(describe(read.findings));
  REQUIRE(read.definition);
  return read.definition;
}

// Harbor Bay moved to Mexico City, placed in Latin America.
std::shared_ptr<const LocationDefinition> mexico_city() {
  json doc = read_json(fixtures() / "valid" / "harbor-bay.location.json");
  doc.erase("integrity");
  doc["location_key"] = "u-3f9a1c2e:ciudad-de-mexico";
  doc["display"]["name"] = "Ciudad de México";
  doc["world"]["placement"] = {{"kind", "region"},
                               {"region", "latin-america-caribbean"}};
  doc["world"]["geo"] = {
      {"lon_e6", -99133208}, {"lat_e6", 19432608}, {"source", "user-placed"}};
  doc["facets"]["countries"] = json::array({"mx"});
  return definition_from(doc);
}

RosterEntry definition_entry(std::shared_ptr<const LocationDefinition> d) {
  return RosterEntry{.key = d->key,
                     .source = RosterSource::Definition,
                     .classic_number = std::nullopt,
                     .definition = std::move(d)};
}

RosterEntry classic_entry(int number) {
  return RosterEntry{.key = classic_key(core::CityId{number}),
                     .source = RosterSource::Classic,
                     .classic_number = number,
                     .definition = nullptr};
}

const WorldEntry& find(const std::vector<WorldEntry>& entries,
                       std::string_view key) {
  for (const auto& e : entries) {
    if (e.key.str() == key) {
      return e;
    }
  }
  FAIL("no entry " << key);
  return entries.front();
}

} // namespace

TEST_CASE("World map paging follows the page-math oracle",
          "[content][worldmap]") {
  const json cases =
      read_json(fixtures() / "catalog-qa" / "page-math-cases.json");
  for (const auto& c : cases.at("cases")) {
    INFO(c.at("case_id").get<std::string>());
    const auto size = c.at("page_size").get<std::size_t>();
    CHECK(page_count(c.at("n_before").get<std::size_t>(), size) ==
          c.at("pages_before").get<int>());
    CHECK(page_count(c.at("n_after").get<std::size_t>(), size) ==
          c.at("pages_after").get<int>());

    // A list of n_after places in one region, as the browser pages it.
    std::vector<WorldEntry> entries;
    const auto n = c.at("n_after").get<std::size_t>();
    for (std::size_t i = 0; i < n; ++i) {
      WorldEntry w;
      w.key = *ContentKey::parse("qa-synthetic:p" + std::to_string(10000 + i));
      w.name = "Place " + std::to_string(10000 + i);
      w.region = 0;
      entries.push_back(std::move(w));
    }
    WorldBrowser browser(std::move(entries), size);
    browser.show(0);
    browser.page(c.at("page_after").get<int>());
    CHECK(browser.pages() == c.at("pages_after").get<int>());
    CHECK(browser.page() == c.at("page_after").get<int>());
    CHECK(browser.paging_enabled() == c.at("paging_enabled").get<bool>());
    CHECK(browser.list().empty() == c.at("empty_state").get<bool>());
  }
}

TEST_CASE("World map lists match the catalog QA page counts",
          "[content][worldmap]") {
  const json expected =
      read_json(fixtures() / "catalog-qa" / "expected-pages.json");
  for (const auto& cat : expected.at("catalogs")) {
    const std::string name = cat.at("catalog");
    INFO(name);
    const auto entries = catalog_entries(
        read_json(fixtures() / "catalog-qa" / (name + ".catalog.json")));
    CHECK(entries.size() == cat.at("total").get<std::size_t>());
    WorldBrowser desktop(entries, desktop_page_size);
    WorldBrowser phone(entries, phone_page_size);
    for (std::size_t r = 0; r < regions.size(); ++r) {
      const std::string id(regions[r].id);
      INFO(id);
      const auto& want = cat.at("regions").at(id);
      desktop.show(r);
      phone.show(r);
      CHECK(desktop.list().size() == want.at("count").get<std::size_t>());
      CHECK(desktop.pages() == want.at("pages_desktop").get<int>());
      CHECK(phone.pages() == want.at("pages_phone").get<int>());
      CHECK(desktop.list().empty() == want.at("empty_state").get<bool>());
      if (!desktop.list().empty()) {
        CHECK(desktop.list().front()->key.str() ==
              want.at("first_key").get<std::string>());
        CHECK(desktop.list().back()->key.str() ==
              want.at("last_key").get<std::string>());
        desktop.page(desktop.pages());
        phone.page(phone.pages());
        CHECK(desktop.page_entries().size() ==
              want.at("last_page_items_desktop").get<std::size_t>());
        CHECK(phone.page_entries().size() ==
              want.at("last_page_items_phone").get<std::size_t>());
      }
    }
    desktop.show(WorldBrowser::bonus_view);
    std::size_t bonus = 0;
    for (const auto& [route, want] : cat.at("bonus_routes").items()) {
      bonus += want.at("count").get<std::size_t>();
    }
    CHECK(desktop.list().size() == bonus);
    CHECK(page_count(entries.size(), desktop_page_size) ==
          cat.at("all_locations").at("pages_desktop").get<int>());
    CHECK(page_count(entries.size(), phone_page_size) ==
          cat.at("all_locations").at("pages_phone").get<int>());
  }
}

TEST_CASE("Previous and next walk the list without wrapping",
          "[content][worldmap]") {
  std::vector<WorldEntry> entries;
  for (int i = 0; i < 13; ++i) {
    WorldEntry w;
    w.key = *ContentKey::parse("qa-synthetic:p" + std::to_string(100 + i));
    w.name = "Place " + std::to_string(100 + i);
    w.region = 2;
    entries.push_back(std::move(w));
  }
  WorldBrowser b(std::move(entries), desktop_page_size);
  b.show(2);
  CHECK_FALSE(b.can_previous());
  REQUIRE(b.next());
  CHECK(b.selected()->name == "Place 100");
  for (int i = 0; i < 12; ++i) {
    REQUIRE(b.next());
  }
  CHECK(b.selected()->name == "Place 112");
  CHECK(b.page() == 2); // the page follows the selection
  CHECK_FALSE(b.next());
  CHECK_FALSE(b.can_next());
  REQUIRE(b.previous());
  CHECK(b.page() == 1);
  b.show(3); // a region without the selection
  CHECK(b.selected() == nullptr);
  CHECK(b.page() == 0);
}

TEST_CASE("Places belong to the region of their placement",
          "[content][worldmap]") {
  const auto mexico = mexico_city();
  const auto village = content_test::load_village();
  const auto roster = Roster::from_entries({classic_entry(0), classic_entry(4),
                                            definition_entry(mexico),
                                            definition_entry(village)});
  REQUIRE(roster.has_value());
  const auto entries = roster_world_entries(*roster);
  REQUIRE(entries.size() == 4);

  const auto north_america = *region_index("north-america");
  const auto latin_america = *region_index("latin-america-caribbean");
  CHECK(find(entries, "classic:paris").region == region_index("europe"));
  CHECK(find(entries, "classic:chicago").region == north_america);
  // Mexico is in Latin America, though its point also lies in the North
  // American crop: it is listed there once and found from the neighbour.
  const auto& m = find(entries, "u-3f9a1c2e:ciudad-de-mexico");
  CHECK(m.region == latin_america);
  CHECK(m.city == core::CityId{2});
  CHECK(std::find(m.tags.begin(), m.tags.end(), "mx") != m.tags.end());
  const auto& v = find(entries, "pthd:santas-pizza-village");
  CHECK_FALSE(v.region.has_value());
  CHECK(v.bonus_route == "north-pole");
  CHECK(std::find(v.tags.begin(), v.tags.end(), "fictional-winter") !=
        v.tags.end());

  WorldBrowser b(entries, desktop_page_size);
  CHECK(b.count_in(latin_america) == 1);
  CHECK(b.count_in(north_america) == 1);
  CHECK(b.count_in(WorldBrowser::bonus_view) == 1);
  b.show(north_america);
  REQUIRE(b.list().size() == 1);
  CHECK(b.list().front()->key.str() == "classic:chicago");
  bool mexico_marker = false;
  for (const auto& marker : b.markers()) {
    for (const auto* e : marker.members) {
      mexico_marker = mexico_marker || e->key == m.key;
    }
  }
  CHECK(mexico_marker);
  b.show(latin_america);
  CHECK(b.markers().size() == 1);
}

TEST_CASE("Markers cluster to the overview's budget", "[content][worldmap]") {
  std::vector<WorldEntry> entries;
  for (int i = 0; i < 40; ++i) {
    WorldEntry w;
    w.key = *ContentKey::parse("qa-synthetic:m" + std::to_string(100 + i));
    w.name = "Marker " + std::to_string(100 + i);
    w.region = 2;
    w.geo = GeoPoint{.lon_e6 = -20000000 + (i * 1700000),
                     .lat_e6 = 35000000 + ((i * 7) % 35) * 1000000};
    entries.push_back(std::move(w));
  }
  const auto few = region_markers({entries.begin(), entries.begin() + 9}, 2);
  CHECK(few.size() == 9);
  const auto many = region_markers(entries, 2);
  CHECK(many.size() <= marker_budget);
  std::size_t members = 0;
  for (const auto& marker : many) {
    members += marker.members.size();
    CHECK(marker.at.x >= 0);
    CHECK(marker.at.x <= 10000);
  }
  CHECK(members == entries.size());
  CHECK(region_markers(entries, 2).size() == many.size()); // deterministic
}

TEST_CASE("Fares come from the coordinates and are deterministic",
          "[content][worldmap]") {
  const auto entries = roster_world_entries(Roster::classic());
  const auto& paris = find(entries, "classic:paris");
  const auto& berlin = find(entries, "classic:berlin");
  const TravelRates rates;
  // (11,052,732 + 3,663,395) micro-degrees at $4 a degree: $58, times
  // difficulty + 1 as on the original map (59 map units there).
  CHECK(travel_cost(paris, berlin, rates, 3) == Money::from_dollars(58 * 4));
  CHECK(travel_cost(berlin, paris, rates, 3) ==
        travel_cost(paris, berlin, rates, 3));
  CHECK(travel_cost(paris, paris, rates, 3) == Money{0});
  for (int i = 0; i < 3; ++i) {
    CHECK(travel_cost(paris, berlin, rates, 1) == Money::from_dollars(58 * 2));
  }

  WorldEntry east;
  east.key = *ContentKey::parse("qa-synthetic:east");
  east.geo = GeoPoint{.lon_e6 = 179000000, .lat_e6 = 0};
  WorldEntry west;
  west.key = *ContentKey::parse("qa-synthetic:west");
  west.geo = GeoPoint{.lon_e6 = -179000000, .lat_e6 = 0};
  // Across the date line, the short way round: 2 degrees.
  CHECK(travel_cost(east, west, rates, 0) == Money::from_dollars(8));

  WorldEntry pole;
  pole.key = *ContentKey::parse("pthd:santas-pizza-village");
  CHECK(travel_cost(paris, pole, rates, 2) == Money::from_dollars(600 * 3));

  Scenario s;
  s.travel_dollars_per_degree_milli = 10000;
  s.bonus_route_cost_dollars = 100;
  const auto custom = travel_rates(&s);
  CHECK(travel_cost(paris, berlin, custom, 0) == Money::from_dollars(147));
  CHECK(travel_cost(paris, pole, custom, 0) == Money::from_dollars(100));
  CHECK(travel_rates(nullptr).dollars_per_degree_milli == 4000);
}

TEST_CASE("Browsing the world map leaves the game as it was",
          "[content][worldmap][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  content_test::start_village_game(game, 77, content_test::load_cozy_village(),
                                   content_test::load_village());
  auto& state = game.state;
  content_test::close_dialogs(state);
  const auto before =
      save::SaveGameManager::fingerprint(state, game.context).hash;
  const auto rng = engine::random::get_state();
  const auto blocks = state.time().time_blocks();

  WorldBrowser b(roster_world_entries(state.roster()), desktop_page_size);
  for (std::size_t v = 0; v < WorldBrowser::view_count; ++v) {
    b.show(v);
    while (b.next()) {
    }
    while (b.previous()) {
    }
    b.page(b.pages());
    (void)b.markers();
  }
  const auto& entries = b.entries();
  for (const auto& from : entries) {
    for (const auto& to : entries) {
      (void)travel_cost(from, to, travel_rates(state.scenario()),
                        state.difficulty_level());
    }
  }

  CHECK(engine::random::get_state() == rng);
  CHECK(state.time().time_blocks() == blocks);
  CHECK(save::SaveGameManager::fingerprint(state, game.context).hash == before);
  // The village has travel off, so its game shows no travel at all.
  CHECK_FALSE(state.travel_enabled());
}
