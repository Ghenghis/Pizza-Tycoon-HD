/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The World Builder's headless half (WORLD-BUILDER-EXPANSION.md): Add Area's
// Automatic generator and the territories' area catalog. No game data is
// needed: the style is the shipped village document.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ContentTestGame.h"
#include "core/content/AreaCatalog.h"
#include "core/content/AreaGenerator.h"
#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"

using namespace core::content;

namespace {

AreaRequest frostfall(std::uint64_t seed) {
  AreaRequest r;
  r.territory = "north-pole";
  r.style = content_test::load_village();
  r.name = "Frostfall Hollow";
  r.seed = seed;
  return r;
}

// Reads the document back as a game would and runs every location rule.
LocationReport reread(const nlohmann::json& document) {
  return check_location_text(document.dump());
}

int business_premises(const LocationDefinition& d) {
  return static_cast<int>(std::count_if(
      d.properties.begin(), d.properties.end(),
      [](const auto& p) { return p.kind == PropertyType::BUSINESS_PREMISE; }));
}

std::vector<std::string> page_keys(const AreaCatalog& c) {
  std::vector<std::string> out;
  for (const auto& p : c.pages("north-pole")) {
    out.push_back(std::to_string(p.page) + " " + p.area_id + " " + p.key.str() +
                  " " + p.name);
  }
  return out;
}

} // namespace

TEST_CASE("Add Area: an Automatic North Pole area passes every location rule",
          "[content][world-builder]") {
  for (const std::uint64_t seed :
       {1ULL, 7ULL, 1224ULL, 23162ULL, 18446744073709551615ULL}) {
    INFO("seed " << seed);
    const AreaProposal p = generate_area(frostfall(seed));
    INFO(describe(p.findings));
    REQUIRE(p.generated());
    CHECK_FALSE(has_errors(p.findings));
    CHECK(p.location_key == "u-00000000:north-pole-frostfall-hollow");
    CHECK(p.document_id == "u-00000000.north-pole-frostfall-hollow");

    const LocationReport report = reread(p.document);
    INFO(describe(report.findings));
    REQUIRE(report.ok());
    const LocationDefinition& d = *report.definition;
    CHECK(d.name == "Frostfall Hollow");
    CHECK(d.revision == 1);
    CHECK(d.bonus);
    CHECK(d.bonus_route == "north-pole");
    CHECK(territory_of(d) == "north-pole");
    // Admitted as written: the declared hash is the computed one.
    REQUIRE(d.declared_sha256.has_value());
    CHECK(*d.declared_sha256 == d.sha256);
    // The village's mix: well over the sixteen premises the rules ask for.
    CHECK(business_premises(d) >= 40);
    CHECK(d.districts.size() == 6);
    CHECK(d.food_suppliers.size() ==
          frostfall(seed).style->food_suppliers.size());
    // The art is the territory's atlases, by reference; the kit is the
    // village's.
    CHECK(d.tile_atlas == "north-pole:gfx/landsym");
    CHECK(d.medium_atlas == "north-pole:gfx/med1");
    CHECK(p.document.at("assets").at("kit").at("kit_id") ==
          "kit:north-pole-village");
    const auto& gen = p.document.at("provenance").at("generator");
    CHECK(p.document.at("provenance").at("origin") == "generated");
    CHECK(gen.at("seed") == std::to_string(seed));
    CHECK(gen.at("template").at("sha256") == frostfall(seed).style->sha256);
  }
}

TEST_CASE("Add Area: the same seed gives the same area, another seed another",
          "[content][world-builder]") {
  const AreaProposal a = generate_area(frostfall(7));
  const AreaProposal b = generate_area(frostfall(7));
  const AreaProposal c = generate_area(frostfall(8));
  REQUIRE(a.generated());
  REQUIRE(b.generated());
  REQUIRE(c.generated());
  CHECK(a.document.dump() == b.document.dump());
  CHECK(a.document.at("tiles") != c.document.at("tiles"));
  // The key comes from the name, not the seed.
  CHECK(a.location_key == c.location_key);
}

TEST_CASE("Add Area: keys never clash, names are checked",
          "[content][world-builder]") {
  AreaRequest r = frostfall(7);
  r.taken_keys = {"u-00000000:north-pole-frostfall-hollow",
                  "u-00000000:north-pole-frostfall-hollow-2"};
  const AreaProposal p = generate_area(r);
  REQUIRE(p.generated());
  CHECK(p.location_key == "u-00000000:north-pole-frostfall-hollow-3");

  for (const std::string& bad :
       {std::string(""), std::string(25, 'a'), std::string("Tab\there")}) {
    AreaRequest q = frostfall(7);
    q.name = bad;
    const AreaProposal refused = generate_area(q);
    CHECK_FALSE(refused.generated());
    CHECK(has_rule(refused.findings, "AREA-1"));
  }
  CHECK(valid_area_name("Caf\xc3\xa9 Glacier")); // UTF-8 for Latin-1 e-acute
  CHECK_FALSE(valid_area_name("Snow \xe2\x98\x83")); // not Latin-1
}

TEST_CASE("Add Area: an Empty draft lists what it needs before Play",
          "[content][world-builder]") {
  AreaRequest r = frostfall(7);
  r.mode = AreaMode::Empty;
  const AreaProposal p = generate_area(r);
  REQUIRE(p.generated());
  CHECK(p.document.at("provenance").at("origin") == "manual");
  CHECK_FALSE(p.document.contains("integrity"));
  const auto missing = missing_requirements(p.document);
  CHECK_FALSE(missing.empty());
  CHECK_FALSE(reread(p.document).ok());
}

TEST_CASE("Area catalog: pages, rename and refusals",
          "[content][world-builder]") {
  AreaCatalog catalog;
  catalog.add_territory(content_test::load_village());
  REQUIRE(catalog.has_territory("north-pole"));
  REQUIRE(catalog.pages("north-pole").size() == 1);

  const auto first = catalog.add_area("north-pole", "Frostfall Hollow", 7,
                                      AreaMode::Automatic);
  INFO(describe(first.findings));
  REQUIRE(first.area);
  const std::string first_id = first.area->id;
  const ContentKey first_key = first.area->key;
  CHECK(first.area->state == AreaState::Admitted);
  REQUIRE(first.area->definition);

  const auto second = catalog.add_area("north-pole", "Frostfall Hollow",
                                       std::nullopt, AreaMode::Automatic);
  REQUIRE(second.area);
  const std::string second_id = second.area->id;
  CHECK(second_id != first_id);
  CHECK(second.area->key != first_key);
  CHECK(second.area->key.str() == "u-00000000:north-pole-frostfall-hollow-2");

  const auto pages = catalog.pages("north-pole");
  REQUIRE(pages.size() == 3);
  CHECK(pages[0].page == 1);
  CHECK(pages[0].area_id.empty()); // the home location
  CHECK(pages[0].key.str() == "pthd:santas-pizza-village");
  CHECK(pages[1].area_id == first_id);
  CHECK(pages[2].area_id == second_id);

  // Rename: the same id and key, the next revision.
  const auto before = catalog.find(first_id)->definition;
  std::string why;
  REQUIRE(catalog.rename(first_id, "Frostfall Harbour", &why));
  const Area* renamed = catalog.find(first_id);
  REQUIRE(renamed);
  CHECK(renamed->name == "Frostfall Harbour");
  CHECK(renamed->key == first_key);
  REQUIRE(renamed->definition);
  CHECK(renamed->definition->revision == before->revision + 1);
  CHECK(renamed->definition->name == "Frostfall Harbour");
  CHECK(before->name == "Frostfall Hollow"); // a holder keeps its copy
  CHECK_FALSE(catalog.rename(first_id, std::string(30, 'x'), &why));
  CHECK(catalog.find(first_id)->name == "Frostfall Harbour");

  // A failed generation leaves the catalog as it was.
  const auto listed = page_keys(catalog);
  const auto bad = catalog.add_area("north-pole", "", 7, AreaMode::Automatic);
  CHECK_FALSE(bad.area);
  CHECK(has_rule(bad.findings, "AREA-1"));
  const auto unknown =
      catalog.add_area("atlantis", "Deep Town", 7, AreaMode::Automatic);
  CHECK_FALSE(unknown.area);
  CHECK(has_rule(unknown.findings, "AREA-2"));
  CHECK(page_keys(catalog) == listed);

  // So does a store that refuses it.
  catalog.hooks(AreaStoreHooks{
      .save_draft = [](const nlohmann::json&, std::optional<std::string>,
                       std::string* error) -> std::optional<std::string> {
        *error = "the disk is full";
        return std::nullopt;
      }});
  const auto refused =
      catalog.add_area("north-pole", "Ice Floe", 3, AreaMode::Automatic);
  CHECK_FALSE(refused.area);
  CHECK(has_rule(refused.findings, "AREA-5"));
  CHECK(page_keys(catalog) == listed);
  catalog.hooks({});

  // An empty draft is listed, and cannot be played.
  const auto draft =
      catalog.add_area("north-pole", "Blank Slate", 1, AreaMode::Empty);
  REQUIRE(draft.area);
  CHECK(draft.area->state == AreaState::Draft);
  CHECK_FALSE(draft.area->definition);
  CHECK_FALSE(draft.missing.empty());
  CHECK_FALSE(catalog.play_problems(draft.area->id).empty());
  CHECK(catalog.play_problems(first_id).empty());
  CHECK(catalog.pages("north-pole").size() == 4);
  CHECK(catalog.pages("north-pole")[3].state == AreaState::Draft);
}
