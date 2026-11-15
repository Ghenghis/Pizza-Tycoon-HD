/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// LocationDefinition reading and validation (location-contract.md sections 3
// to 5), the scenario documents (section 11), the first shipped content
// (Santa's Pizza Village) and building a city from a definition without the
// simulation stream.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "core/character/Actor.h"
#include "core/city/City.h"
#include "core/city/District.h"
#include "core/city/Property.h"
#include "core/common/Ids.h"
#include "core/content/DefinitionCitySource.h"
#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"
#include "core/content/Roster.h"
#include "core/content/Scenario.h"
#include "core/content/ScenarioSave.h"
#include "core/content/Sha256.h"
#include "core/content/ShippedScenarios.h"
#include "core/events/MilestoneId.h"
#include "core/init/NewGameConfig.h"
#include "core/state/GameRules.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace core::content;

namespace {

fs::path tests_dir() { return fs::path(__FILE__).parent_path(); }
fs::path fixtures_dir() { return tests_dir() / "fixtures" / "locations"; }
fs::path data_dir() { return tests_dir().parent_path() / "data"; }
fs::path village_path() {
  return data_dir() / "locations" / "north-pole" / "santas-pizza-village.json";
}
fs::path cozy_village_path() {
  return data_dir() / "scenarios" / "north-pole-cozy-village.json";
}

json read_json(const fs::path& path) {
  std::ifstream in(path);
  REQUIRE(in.good());
  return json::parse(in);
}

// Both stages on a document held in memory.
Findings check(const json& document) {
  auto read = read_location(document);
  Findings findings = read.findings;
  if (read.definition) {
    const auto semantic = validate_location(*read.definition);
    findings.insert(findings.end(), semantic.begin(), semantic.end());
  }
  return findings;
}

bool has_finding_at(const Findings& findings, std::string_view rule,
                    std::string_view pointer) {
  return std::any_of(findings.begin(), findings.end(), [&](const Finding& f) {
    return f.rule == rule && f.pointer == pointer;
  });
}

std::string hex_byte(std::uint8_t value) {
  constexpr std::string_view digits = "0123456789abcdef";
  return {digits[value >> 4], digits[value & 0x0F]};
}

// Sets one tile of a document's map.
void set_tile(json& doc, int x, int y, std::uint8_t code) {
  auto row = doc["tiles"][static_cast<std::size_t>(y)].get<std::string>();
  row.replace(static_cast<std::size_t>(x) * 2, 2, hex_byte(code));
  doc["tiles"][static_cast<std::size_t>(y)] = row;
}

json harbor_bay() {
  return read_json(fixtures_dir() / "valid" / "harbor-bay.location.json");
}

// The harbor fixture without its integrity block, so edits stay valid.
json editable_harbor_bay() {
  json doc = harbor_bay();
  doc.erase("integrity");
  return doc;
}

std::shared_ptr<const LocationDefinition> village() {
  auto report = load_location(village_path());
  INFO(describe(report.findings));
  REQUIRE(report.ok());
  return report.definition;
}

} // namespace

TEST_CASE("SHA-256 gives the FIPS 180-4 digests", "[content]") {
  CHECK(Sha256::hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256::hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(
      Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Fed in pieces across block boundaries, the digest is the same.
  const std::string million(1'000'000, 'a');
  Sha256 pieces;
  for (std::size_t i = 0; i < million.size(); i += 777) {
    pieces.update(std::string_view(million).substr(i, 777));
  }
  const auto digest = pieces.finish();
  CHECK(digest[0] == 0xcd);
  CHECK(digest[31] == 0xd0);
  CHECK(Sha256::hex(million) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Location fixtures pass or fail exactly as expectations.json says",
          "[content][definition]") {
  const json expectations = read_json(fixtures_dir() / "expectations.json");
  int checked = 0;
  for (const auto& fixture : expectations.at("fixtures")) {
    if (fixture.at("schema") != "location.schema.json") {
      continue;
    }
    const std::string path = fixture.at("path");
    INFO(path);
    const auto report = load_location(fixtures_dir() / path);
    INFO(describe(report.findings));
    if (fixture.at("expect") == "pass") {
      CHECK(report.ok());
      CHECK_FALSE(has_errors(report.findings));
    }
    else {
      CHECK_FALSE(report.ok());
      const auto expected = fixture.at("rules").get<std::vector<std::string>>();
      CHECK(error_rules(report.findings) == expected);
      if (fixture.contains("pointer")) {
        CHECK(has_finding_at(report.findings, "SCHEMA-1",
                             fixture.at("pointer").get<std::string>()));
      }
    }
    ++checked;
  }
  CHECK(checked == 10);
}

TEST_CASE("Scenario fixtures pass or fail exactly as expectations.json says",
          "[content][scenario]") {
  const json expectations = read_json(fixtures_dir() / "expectations.json");
  int checked = 0;
  for (const auto& fixture : expectations.at("fixtures")) {
    if (fixture.at("schema") != "scenario.schema.json") {
      continue;
    }
    const std::string path = fixture.at("path");
    INFO(path);
    const auto result = read_scenario_file(fixtures_dir() / path);
    INFO(describe(result.findings));
    if (fixture.at("expect") == "pass") {
      CHECK(result.ok());
    }
    else {
      CHECK_FALSE(result.ok());
      const auto expected = fixture.at("rules").get<std::vector<std::string>>();
      CHECK(error_rules(result.findings) == expected);
      if (fixture.contains("pointer")) {
        CHECK(has_finding_at(result.findings, "SCHEMA-1",
                             fixture.at("pointer").get<std::string>()));
      }
    }
    ++checked;
  }
  CHECK(checked == 5);
}

TEST_CASE("A definition's hash is its canonical form without integrity",
          "[content][definition]") {
  const json doc = harbor_bay();
  const auto read = read_location(doc);
  REQUIRE(read.definition);
  REQUIRE(read.definition->declared_sha256.has_value());
  CHECK(read.definition->sha256 == *read.definition->declared_sha256);
  CHECK(read.definition->sha256 == definition_sha256(doc));
  // Member order and whitespace are not part of the hash.
  CHECK(definition_sha256(json::parse(doc.dump(3))) == read.definition->sha256);

  json tampered = doc;
  tampered["display"]["name"] = "Harbour Bay";
  const auto findings = check(tampered);
  CHECK(error_rules(findings) == std::vector<std::string>{"INT-1"});
}

TEST_CASE("The reader reports shape errors with their JSON pointer",
          "[content][definition]") {
  SECTION("a missing member") {
    json doc = editable_harbor_bay();
    doc["properties"][3].erase("realtor");
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/properties/3/realtor"));
  }
  SECTION("an unknown member") {
    json doc = editable_harbor_bay();
    doc["districts"][1]["mood"] = "festive";
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/districts/1/mood"));
  }
  SECTION("a value of the wrong type") {
    json doc = editable_harbor_bay();
    doc["actors"]["banks"][0]["head"] = "two";
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/actors/banks/0/head"));
  }
  SECTION("a head the actor views cannot draw") {
    json doc = editable_harbor_bay();
    doc["food_suppliers"][0]["head"] = 26;
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/food_suppliers/0/head"));
  }
  SECTION("a reserved namespace") {
    json doc = editable_harbor_bay();
    doc["location_key"] = "classic:harbor-bay";
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/location_key"));
  }
  SECTION("a name outside the original fonts") {
    json doc = editable_harbor_bay();
    doc["display"]["name"] = "Harbor Ładź";
    CHECK(has_finding_at(check(doc), "SCHEMA-1", "/display/name"));
  }
  SECTION("not JSON at all") {
    const auto report = check_location_text("{\"format\": ");
    CHECK(error_rules(report.findings) == std::vector<std::string>{"SCHEMA-1"});
  }
}

TEST_CASE("Asset keys: classic, a location's own art, or a bundle",
          "[content][definition][assets]") {
  const auto with_key = [](const std::string& key) {
    json doc = editable_harbor_bay();
    doc["assets"]["tile_atlas"]["key"] = key;
    return doc;
  };
  for (const char* good :
       {"classic:gfx/landsym", "classic:gfx/anything_16_char",
        "bundle:art/x.png", "north-pole:gfx/landsym", "north-pole:gfx/med1",
        "np_2:gfx/k0", "harbor-bay:gfx/back12", "a:gfx/kitchen",
        "classic:gfx/med1"}) {
    INFO(good);
    const auto read = read_location(with_key(good));
    INFO(describe(read.findings));
    REQUIRE(read.definition);
    CHECK(read.definition->tile_atlas == good);
    CHECK_FALSE(
        has_finding_at(read.findings, "SCHEMA-1", "/assets/tile_atlas/key"));
  }
  for (const char* bad :
       {"gfx/landsym",            // no namespace
        "North-Pole:gfx/landsym", // capitals
        "north pole:gfx/med1",    // a space
        "north-pole:sfx/med1",    // not a picture
        "north-pole:gfx/",        // no picture
        "north-pole:gfx/menu",    // not a picture a location can bind
        "north-pole:gfx/k",       // k* needs more
        "north-pole:gfx/landsym2", "north-pole:gfx/../landsym", ":gfx/landsym",
        "abcdefghijklmnopqrstuvwxyz0123456:gfx/landsym", "classic:gfx/",
        "classic:gfx/Landsym", "bundle:", "bundle:a b"}) {
    INFO(bad);
    const auto findings = check(with_key(bad));
    CHECK(has_finding_at(findings, "SCHEMA-1", "/assets/tile_atlas/key"));
    // The message names the key and what is wrong with it.
    const auto it = std::ranges::find_if(findings, [](const Finding& f) {
      return f.pointer == "/assets/tile_atlas/key";
    });
    REQUIRE(it != findings.end());
    CHECK(it->message.find(bad) != std::string::npos);
  }
}

TEST_CASE("A location's own art key survives the save's embedded definition",
          "[content][definition][assets][save]") {
  const auto definition = village();
  REQUIRE(definition->tile_atlas == "north-pole:gfx/landsym");
  // The canonical document read back is the same definition.
  const auto again = read_location(json::parse(definition->canonical_document));
  INFO(describe(again.findings));
  REQUIRE(again.definition);
  CHECK(again.definition->tile_atlas == definition->tile_atlas);
  CHECK(again.definition->medium_atlas == definition->medium_atlas);
  CHECK(again.definition->sha256 == definition->sha256);

  // Through a version 4 save's scenario block and back.
  auto roster =
      Roster::from_entries({RosterEntry{.key = definition->key,
                                        .source = RosterSource::Definition,
                                        .classic_number = std::nullopt,
                                        .definition = definition}});
  REQUIRE(roster);
  const json block = save_scenario_block(*roster, nullptr);
  std::string error;
  const auto saved = read_scenario_block(json::parse(block.dump()), error);
  INFO(error);
  REQUIRE(saved);
  const auto& entry = saved->roster.entry(core::CityId{0});
  REQUIRE(entry.definition);
  CHECK(entry.definition->tile_atlas == "north-pole:gfx/landsym");
  CHECK(entry.definition->medium_atlas == "north-pole:gfx/med1");
  CHECK(entry.definition->sha256 == definition->sha256);
}

TEST_CASE("The semantic rules catch what the schema cannot",
          "[content][definition]") {
  SECTION("QA content is never built") {
    json doc = editable_harbor_bay();
    doc["location_key"] = "qa-synthetic:d35-r1-0001";
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"ID-3"});
  }
  SECTION("a broken lane is a dead end without a return lane") {
    json doc = editable_harbor_bay();
    set_tile(doc, 12, 58, 0x00); // the west lane of the avenue
    CHECK(error_rules(check(doc)) ==
          std::vector<std::string>{"ROAD-2", "ROAD-4"});
  }
  SECTION("0x77 is crossed in a straight line, as in the original maps") {
    json doc = editable_harbor_bay();
    set_tile(doc, 12, 58, 0x77);
    set_tile(doc, 120, 59, 0x77);
    CHECK_FALSE(has_errors(check(doc)));
  }
  SECTION("two unknown road codes are warnings only") {
    json doc = editable_harbor_bay();
    set_tile(doc, 5, 5, 0xE6);
    const auto findings = check(doc);
    CHECK_FALSE(has_errors(findings));
    CHECK(has_rule(findings, "TILE-2"));
  }
  SECTION("a property needs a premise tile") {
    json doc = editable_harbor_bay();
    doc["properties"][0]["tile"] = json{{"x", 3}, {"y", 3}};
    CHECK(error_rules(check(doc)) ==
          std::vector<std::string>{"PROP-1", "PROP-3"});
  }
  SECTION("too few premises for the AI players") {
    json doc = editable_harbor_bay();
    // Turn eight shop fronts into grass and drop their properties.
    int removed = 0;
    auto& props = doc["properties"];
    for (std::size_t i = props.size(); i-- > 0 && removed < 8;) {
      if (props[i]["kind"] != "business_premise") {
        continue;
      }
      set_tile(doc, props[i]["tile"]["x"].get<int>(),
               props[i]["tile"]["y"].get<int>(), 0x00);
      props.erase(i);
      ++removed;
    }
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"PROP-5"});
  }
  SECTION("an unaffordable price level overflows") {
    json doc = editable_harbor_bay();
    doc["districts"][0]["price_level"] = 20000;
    CHECK(has_rule(check(doc), "ECON-2"));
  }
  SECTION("a supplier number twice") {
    json doc = editable_harbor_bay();
    doc["food_suppliers"][1]["number"] = doc["food_suppliers"][0]["number"];
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"ID-2"});
  }
  SECTION("a premise sold by someone who is no realtor") {
    json doc = editable_harbor_bay();
    doc["properties"][2]["realtor"] = "a-bank-1";
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"REF-1"});
  }
  SECTION("art outside the bundle and unapproved art") {
    json doc = editable_harbor_bay();
    doc["assets"]["attachments"] = json::array({json{
        {"id", "thumb"},
        {"role", "thumbnail"},
        {"file",
         {{"path", "art/thumb.png"},
          {"sha256", std::string(64, 'a')},
          {"bytes", 10},
          {"width", 8},
          {"height", 8}}},
        {"state", "quarantine"},
        {"source", {{"type", "generated"}, {"rights_status", "local_only"}}}}});
    doc["assets"]["thumbnail"] = json{{"attachment", "missing"}};
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"ASSET-2"});
    doc["assets"]["thumbnail"] = json{{"attachment", "thumb"}};
    CHECK_FALSE(has_errors(check(doc))); // not admitted yet
    doc["integrity"] = json{{"canonicalization", "jcs-rfc8785"},
                            {"definition_sha256", definition_sha256(doc)}};
    CHECK(error_rules(check(doc)) == std::vector<std::string>{"ASSET-2"});
  }
  SECTION("a marker outside its region's overview") {
    json doc = editable_harbor_bay();
    doc["world"]["geo"]["lon_e6"] = 120000000;
    const auto findings = check(doc);
    CHECK_FALSE(has_errors(findings));
    CHECK(has_rule(findings, "WORLD-1"));
  }
}

TEST_CASE("Santa's Pizza Village is a valid location",
          "[content][definition][village]") {
  const auto report = load_location(village_path());
  INFO(describe(report.findings));
  REQUIRE(report.ok());
  // Shipped content carries no warnings either.
  CHECK(report.findings.empty());

  const auto& v = *report.definition;
  CHECK(v.key.str() == "pthd:santas-pizza-village");
  CHECK(v.name == "Santa's Pizza Village");
  CHECK(v.bonus);
  CHECK(v.bonus_route == "north-pole");
  CHECK_FALSE(v.geo.has_value());
  REQUIRE(v.tiles.size() == static_cast<std::size_t>(grid_tiles));
  CHECK(v.declared_sha256 == v.sha256);
  // The village's own art, in the classic layout (views/hd/LocationArt).
  CHECK(v.tile_atlas == "north-pole:gfx/landsym");
  CHECK(v.medium_atlas == "north-pole:gfx/med1");

  int business = 0;
  int depots = 0;
  for (const auto& p : v.properties) {
    if (p.kind == PropertyType::BUSINESS_PREMISE) {
      ++business;
    }
    else {
      ++depots;
    }
    CHECK(p.entrance_offset >= 1);
    CHECK(p.entrance_offset <= p.interior_w - 2);
  }
  CHECK(business >= 16);
  CHECK(depots == 1);
  CHECK(v.districts.size() >= 4);
  CHECK(v.actors.mayor.has_value());
  CHECK(v.actors.realtors.size() >= 2);

  // Every tile is a code of the original landsym vocabulary (revision 3:
  // the districts' own tiles, zebra crossings and shadowed lanes too).
  const std::set<std::uint8_t> vocabulary = {
      0x00, 0x04, 0x06, 0x09, 0x10, 0x14, 0x16, 0x17, 0x19, 0x20, 0x24, 0x26,
      0x29, 0x30, 0x34, 0x36, 0x39, 0x40, 0x43, 0x46, 0x49, 0x50, 0x53, 0x56,
      0x59, 0x60, 0x64, 0x66, 0x69, 0x70, 0x74, 0x76, 0x79, 0x80, 0x86, 0x88,
      0x89, 0x8A, 0x90, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9F, 0xA0, 0xA6, 0xA8,
      0xAA, 0xAB, 0xB6, 0xB8, 0xBA, 0xBB, 0xC1, 0xC7, 0xC8, 0xCA, 0xCB, 0xD1,
      0xD7, 0xD8, 0xDA, 0xDB, 0xE4, 0xE7, 0xE8, 0xEA, 0xF3, 0xF8, 0xFA, 0xFB};
  for (const auto t : v.tiles) {
    INFO("tile code " << static_cast<int>(t));
    CHECK(vocabulary.contains(t));
  }
}

TEST_CASE("The Cozy Village scenario starts a sandbox in the village",
          "[content][scenario][village]") {
  const auto read = read_scenario_file(cozy_village_path());
  INFO(describe(read.findings));
  REQUIRE(read.ok());
  const auto& s = *read.scenario;
  CHECK(s.id.str() == "pthd:north-pole-cozy-village");
  CHECK(s.win_mode == WinMode::None);
  CHECK(s.after_goal == AfterGoal::ContinueSandbox);
  CHECK(s.start == ScenarioStart::QuickStart);
  CHECK(s.quickstart_big);
  CHECK(s.difficulty == 1);
  CHECK(s.travel_disabled);
  REQUIRE(s.goal.size() == 2);
  CHECK(s.goal[0].milestone == core::events::MilestoneId::HasAssetsOf100K);
  CHECK(s.goal[1].milestone == core::events::MilestoneId::AwardedFirstStar);
  REQUIRE(s.optional_objectives.size() == 2);
  CHECK(s.optional_objectives[1].polarity == Polarity::Avoid);
  CHECK(s.optional_objectives[1].milestone ==
        core::events::MilestoneId::TakesOutFirstLoan);
  REQUIRE(s.roster.size() == 1);
  CHECK(s.roster[0].pin == Pin::Exact);

  const auto definition = village();
  Findings findings;
  const auto roster = scenario_roster(
      s,
      [&](const ContentKey& key) -> std::shared_ptr<const LocationDefinition> {
        return key == definition->key ? definition : nullptr;
      },
      findings);
  INFO(describe(findings));
  REQUIRE(roster.has_value());
  CHECK(roster->size() == 1);
  CHECK(roster->entry(core::CityId{0}).source == RosterSource::Definition);
  CHECK(roster->display_name(core::CityId{0}) == "Santa's Pizza Village");

  init::Config config;
  GameRules rules;
  REQUIRE(configure_new_game(read.scenario, *roster, config, rules, findings));
  CHECK(config.mode == GameMode::QUICKSTART);
  CHECK(config.size == init::RestaurantSize::BIG);
  CHECK(config.difficulty == init::DifficultyLevel::PUSHOVER);
  CHECK(config.city_id == core::CityId{0});
  CHECK(config.roster == *roster);
  CHECK(config.scenario == read.scenario);
  // The scenario's rules travel with the game, not in the player's options.
  REQUIRE(config.rules.has_value());
  CHECK(*config.rules == rules);
}

TEST_CASE("The New Game entry finds the village in the data folder",
          "[content][scenario][village]") {
  init::Config config;
  Findings findings;
  REQUIRE(prepare_shipped_scenario(cozy_village.file, config, GameRules{},
                                   findings, data_dir()));
  INFO(describe(findings));
  CHECK(config.roster.size() == 1);
  CHECK(config.roster.display_name(core::CityId{0}) == north_pole_menu_label);
  CHECK(config.mode == GameMode::QUICKSTART);
  REQUIRE(config.scenario != nullptr);
  CHECK(config.scenario->id.str() == "pthd:north-pole-cozy-village");

  Findings missing;
  CHECK_FALSE(find_shipped_location(*ContentKey::parse("pthd:atlantis"),
                                    missing, data_dir()));
  CHECK(has_rule(missing, "SCN-6"));
  init::Config untouched;
  CHECK_FALSE(prepare_shipped_scenario("no-such-scenario", untouched,
                                       GameRules{}, missing, data_dir()));
  CHECK(untouched.roster.is_classic());
}

TEST_CASE("A scenario's roster holds each definition to its pin",
          "[content][scenario]") {
  const auto read = read_scenario_file(cozy_village_path());
  REQUIRE(read.ok());
  Findings findings;
  SECTION("no definition") {
    CHECK_FALSE(scenario_roster(*read.scenario, nullptr, findings));
    CHECK(error_rules(findings) == std::vector<std::string>{"SCN-6"});
  }
  SECTION("another revision of the definition") {
    auto changed = std::make_shared<LocationDefinition>(*village());
    changed->sha256 = std::string(64, '0');
    CHECK_FALSE(scenario_roster(
        *read.scenario,
        [&](const ContentKey&) -> std::shared_ptr<const LocationDefinition> {
          return changed;
        },
        findings));
    CHECK(error_rules(findings) == std::vector<std::string>{"SCN-6"});
  }
}

TEST_CASE("Building a city from a definition draws no random numbers",
          "[content][definition]") {
  const auto read = read_location(harbor_bay());
  REQUIRE(read.definition);
  const auto& def = *read.definition;

  engine::random::seed(0x5EED);
  const auto before = engine::random::get_state();
  const auto city = build_static_city(core::CityId{3}, def);
  CHECK(engine::random::get_state() == before);

  REQUIRE(city);
  CHECK(city->id() == core::CityId{3});
  CHECK(city->name() == "Harbor Bay");
  CHECK(city->data_length() == grid_tiles);
  REQUIRE(city->districts().size() == def.districts.size());
  CHECK(city->districts()[2]->name() == def.districts[2].name);
  CHECK(city->properties().size() == def.properties.size());

  // Every property exactly as realized.
  for (const auto& p : def.properties) {
    INFO(p.id);
    const auto* property = city->property_by_index(p.tile_index());
    REQUIRE(property != nullptr);
    CHECK(property->size().w == p.interior_w);
    CHECK(property->size().h == p.interior_h);
    CHECK(property->kitchen_size().w == p.kitchen_w);
    CHECK(property->kitchen_size().h == p.kitchen_h);
    CHECK(property->entrance_offset() == p.entrance_offset);
    CHECK(property->kitchen_entry_offset() == p.kitchen_entry_offset);
    CHECK(property->purchase_price().cents == p.purchase_price_cents);
    CHECK(property->type() == p.kind);
  }

  // Actors in Classic id order: realtors, banks, insurers, mayor, police,
  // mafia, arms dealers.
  const auto& a = def.actors;
  const auto realtors = city->actors(ActorType::REALTOR);
  REQUIRE(realtors.size() == a.realtors.size());
  CHECK(realtors[0]->id() == core::ActorId{0});
  REQUIRE(city->mayor() != nullptr);
  CHECK(city->mayor()->name() == a.mayor->name);
  const int mayor_id =
      static_cast<int>(a.realtors.size() + a.banks.size() + a.insurers.size());
  CHECK(city->mayor()->id() == core::ActorId{mayor_id});
  REQUIRE(city->police() != nullptr);
  CHECK(city->police()->name() == a.police.front().name);
  CHECK(city->suppliers().size() == def.food_suppliers.size());
  CHECK(
      city->ingredient_base_price(core::food::IngredientType::TOMATOES).cents ==
      def.ingredient_base_prices[0]);
}

TEST_CASE("A definition city starts up like a Classic one",
          "[content][definition][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());
  const auto definition = village();

  // The start-up draws the simulation stream (advertising, trends, job
  // market) and draws it the same way every time.
  engine::random::seed(1224);
  const auto untouched = engine::random::get_state();
  const auto first = build_city(core::CityId{0}, *definition, true, {});
  const auto after_first = engine::random::get_state();
  CHECK(after_first != untouched);
  engine::random::seed(1224);
  const auto second = build_city(core::CityId{0}, *definition, true, {});
  CHECK(engine::random::get_state() == after_first);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->mayor() != nullptr);
  CHECK(first->business_premises().size() ==
        second->business_premises().size());
}

TEST_CASE("Every North Pole preset plays the same pinned village",
          "[content][scenario][village]") {
  const auto definition = village();
  // The six districts of NORTH-POLE-FLAGSHIP.md, each with premises.
  const std::vector<std::string> districts = {
      "Candy Cane Market", "Toy Workshop Quarter",  "Village Square",
      "Reindeer Ridge",    "Frosty's Snow Gardens", "Aurora Harbor"};
  REQUIRE(definition->districts.size() == districts.size());
  std::vector<int> premises(districts.size(), 0);
  for (const auto& p : definition->properties) {
    if (p.kind == PropertyType::BUSINESS_PREMISE) {
      ++premises[static_cast<std::size_t>(
          definition->district_of_tile(p.x, p.y))];
    }
  }
  for (std::size_t i = 0; i < districts.size(); ++i) {
    INFO(districts[i]);
    CHECK(definition->districts[i].name == districts[i]);
    CHECK(premises[i] >= 3);
  }

  // Each district reads differently (revision 3): leaving out the base
  // grass, roads, premises, named buildings and the depot, its most common
  // tile is its own: shopfronts, workshop roofs, the plaza, meadow, lawns,
  // the frozen harbour.
  std::vector<std::map<std::uint8_t, int>> counts(districts.size());
  for (int y = 0; y < grid_height; ++y) {
    for (int x = 0; x < grid_width; ++x) {
      const std::uint8_t t = definition->tile(x, y);
      const int low = t & 0x0F;
      if (t == 0x60 || t == 0x04 || low == 0x06 || low == 0x07 || low == 0x0A ||
          (low == 0x08 && t >= 0x80)) {
        continue;
      }
      ++counts[static_cast<std::size_t>(definition->district_of_tile(x, y))][t];
    }
  }
  std::set<std::uint8_t> signatures;
  for (std::size_t i = 0; i < districts.size(); ++i) {
    INFO(districts[i]);
    REQUIRE_FALSE(counts[i].empty());
    const auto top =
        std::ranges::max_element(counts[i], [](const auto& a, const auto& b) {
          return a.second < b.second;
        });
    CHECK(signatures.insert(top->first).second);
  }
  CHECK(signatures.size() == districts.size());

  for (const auto& preset : north_pole_presets) {
    INFO(preset.file);
    const auto read = read_scenario_file(data_dir() / "scenarios" /
                                         (std::string(preset.file) + ".json"));
    INFO(describe(read.findings));
    REQUIRE(read.ok());
    const auto& s = *read.scenario;
    REQUIRE(s.roster.size() == 1);
    CHECK(s.roster[0].pin == Pin::Exact);
    CHECK(s.roster[0].revision == definition->revision);
    CHECK(s.roster[0].definition_sha256 == definition->sha256);
    CHECK(s.win_mode == WinMode::None);
    CHECK(s.after_goal == AfterGoal::ContinueSandbox);
    // Every objective is one the observer can read.
    for (const auto& o : s.goal) {
      CHECK(supported_milestone(milestone_name(o.milestone)).has_value());
    }

    init::Config config;
    Findings findings;
    CHECK(prepare_shipped_scenario(preset.file, config, GameRules{}, findings,
                                   data_dir()));
  }

  // Goals by preset: Cozy and Workshop Rush and the Empire have them, the
  // sandbox has none and never announces one.
  const auto goals = [&](const ShippedScenario& preset) {
    return read_scenario_file(data_dir() / "scenarios" /
                              (std::string(preset.file) + ".json"))
        .scenario->goal.size();
  };
  CHECK(goals(cozy_village) == 2);
  CHECK(goals(workshop_rush) == 3);
  CHECK(goals(pizza_empire) == 3);
  CHECK(goals(village_sandbox) == 0);
}

TEST_CASE("A preset without a goal can only continue", "[content][scenario]") {
  json doc = read_json(data_dir() / "scenarios" / "north-pole-sandbox.json");
  CHECK(read_scenario(doc).ok());
  doc["objectives"]["after_goal"] = "end-game";
  CHECK(error_rules(read_scenario(doc).findings) ==
        std::vector<std::string>{"SCN-3"});
}
