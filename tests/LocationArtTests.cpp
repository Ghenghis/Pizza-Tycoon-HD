/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Location-aware art (views/hd/LocationArt.h): Classic cities get exactly
// the 1994 keys and pictures; a location's city asks for its namespaced
// atlas and draws with the HD pack's location art when it is there and
// keeps the rules, the classic picture otherwise.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/content/LocationValidator.h"
#include "core/content/Roster.h"
#include "engine/Logger.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/HdPack.h"
#include "resources/VgaLoader.h"
#include "resources/VgaManager.h"
#include "resources/util/FileUtils.h"
#include "views/hd/LocationArt.h"

namespace fs = std::filesystem;
using core::CityId;
using core::content::Roster;
using core::content::RosterEntry;
using core::content::RosterSource;
using test_support::ScopedHdScale;
using views::hd::ArtScope;
using views::hd::CityAtlas;

namespace {

fs::path data_dir() {
  return fs::path(__FILE__).parent_path().parent_path() / "data";
}

std::shared_ptr<const core::content::LocationDefinition> north_pole() {
  auto report = core::content::load_location(
      data_dir() / "locations" / "north-pole" / "santas-pizza-village.json");
  REQUIRE(report.ok());
  return report.definition;
}

Roster north_pole_roster() {
  auto def = north_pole();
  auto roster =
      Roster::from_entries({RosterEntry{.key = def->key,
                                        .source = RosterSource::Definition,
                                        .classic_number = std::nullopt,
                                        .definition = def}});
  REQUIRE(roster);
  return *roster;
}

// The location pictures and the HD pack as other tests expect them.
struct Clean {
  Clean() { views::hd::clear_location_pictures(); }
  ~Clean() {
    views::hd::clear_location_pictures();
    resources::HdPack::instance().configure({});
  }
  Clean(const Clean&) = delete;
  Clean& operator=(const Clean&) = delete;
};

uint32_t first_texel(const gfx::Surface& s) {
  const gfx::HdImage* hd = s.hd();
  return hd ? hd->at(0, 0) : 0u;
}

} // namespace

TEST_CASE("atlas keys split into scope and picture", "[hd][location]") {
  const auto classic = views::hd::split_atlas_key("classic:gfx/landsym");
  REQUIRE(classic);
  CHECK(classic->scope.classic());
  CHECK(classic->picture == "gfx/landsym");
  const auto np = views::hd::split_atlas_key("north-pole:gfx/med1");
  REQUIRE(np);
  CHECK(np->scope.ns == "north-pole");
  CHECK(np->picture == "gfx/med1");
  for (const char* bad : {"gfx/landsym", "bundle:art/x.png", "North:gfx/x",
                          "np:gfx/../x", "np:sfx/x", ":gfx/x", "np:gfx/"}) {
    INFO(bad);
    CHECK_FALSE(views::hd::split_atlas_key(bad));
  }
}

TEST_CASE("Classic cities resolve to exactly the 1994 keys and pictures",
          "[hd][location]") {
  Clean clean;
  const Roster roster = Roster::classic();
  for (int id = 0; id < 10; ++id) {
    CHECK(views::hd::atlas_key(roster, CityId{id}, CityAtlas::Tiles) ==
          "classic:gfx/landsym");
    CHECK(views::hd::atlas_key(roster, CityId{id}, CityAtlas::Medium) ==
          "classic:gfx/med1");
    CHECK(views::hd::art_scope(roster, CityId{id}, 2).classic());
  }
  // Even with location art in the pack, a Classic scope is the picture
  // itself, the very same object.
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  REQUIRE(gfx::save_png(gfx::HdImage(512, 512, gfx::pack_rgba(1, 2, 3, 255)),
                        dir.path() / "locations" / "north-pole" / "gfx" /
                            "landsym.png"));
  resources::HdPack::instance().configure(dir.path());
  const gfx::Surface classic(512, 512, gfx::PaletteHandle{0}, 7);
  CHECK(&views::hd::location_picture(classic, "gfx/landsym", ArtScope{}) ==
        &classic);
}

TEST_CASE("the North Pole city map asks for its namespaced atlases",
          "[hd][location][content]") {
  Clean clean;
  const Roster roster = north_pole_roster();
  CHECK(views::hd::atlas_key(roster, CityId{0}, CityAtlas::Tiles) ==
        "north-pole:gfx/landsym");
  CHECK(views::hd::atlas_key(roster, CityId{0}, CityAtlas::Medium) ==
        "north-pole:gfx/med1");
  const ArtScope scope = views::hd::art_scope(roster, CityId{0}, 2);
  CHECK(scope.ns == "north-pole");
  CHECK(scope.district == north_pole()->districts[2].id);
  CHECK(views::hd::art_scope(roster, CityId{0}, 99).district.empty());
  CHECK(views::hd::art_scope(roster, CityId{5}).classic()); // not in it
}

TEST_CASE("location art draws from the pack's location folder",
          "[hd][location]") {
  Clean clean;
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const fs::path loc = dir.path() / "locations" / "north-pole";
  const uint32_t snow = gfx::pack_rgba(230, 240, 255, 255);
  const uint32_t candy = gfx::pack_rgba(220, 20, 40, 255);
  const uint32_t cocoa = gfx::pack_rgba(90, 50, 20, 255);
  REQUIRE(
      gfx::save_png(gfx::HdImage(512, 512, snow), loc / "gfx" / "landsym.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(192, 44, cocoa), loc / "gfx" / "k3.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(192, 44, candy),
                        loc / "districts" / "d-candy" / "gfx" / "k3.png"));
  resources::HdPack::instance().configure(dir.path());

  // The 1994 pictures: landsym is 256 x 256 (engine 512), k3 96 x 22.
  const gfx::Surface landsym(512, 512, gfx::PaletteHandle{0}, 7);
  const gfx::Surface k3(192, 44, gfx::PaletteHandle{0}, 9);
  const ArtScope np{.ns = "north-pole", .district = ""};

  const gfx::Surface& tiles =
      views::hd::location_picture(landsym, "gfx/landsym", np);
  REQUIRE(&tiles != &landsym);
  CHECK(first_texel(tiles) == snow);
  // The indexed pixels (the tile ids, the 1994 look) are the classic ones.
  REQUIRE(tiles.width() == landsym.width());
  REQUIRE(tiles.height() == landsym.height());
  CHECK(std::memcmp(tiles.data(), landsym.data(),
                    static_cast<std::size_t>(512 * 512)) == 0);
  // Made once.
  CHECK(&views::hd::location_picture(landsym, "gfx/landsym", np) == &tiles);

  // A district's own floor first, then the location's.
  const ArtScope candy_scope{.ns = "north-pole", .district = "d-candy"};
  const ArtScope ridge_scope{.ns = "north-pole", .district = "d-ridge"};
  CHECK(first_texel(views::hd::location_picture(k3, "gfx/k3", candy_scope)) ==
        candy);
  CHECK(first_texel(views::hd::location_picture(k3, "gfx/k3", ridge_scope)) ==
        cocoa);
  // Nothing in the pack for it: the classic picture.
  const gfx::Surface k4(192, 44, gfx::PaletteHandle{0}, 9);
  CHECK(&views::hd::location_picture(k4, "gfx/k4", np) == &k4);
}

TEST_CASE("a location atlas that breaks the rules falls back to classic",
          "[hd][location]") {
  Clean clean;
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  std::vector<std::string> lines;
  const Logger::ScopedSink sink(Logger::add_sink(
      [&lines](LogLevel, const std::string& msg) {
        if (msg.find("Location art") != std::string::npos) {
          lines.push_back(msg);
        }
      },
      LogLevel::Info));
  // 500 is not a whole number of tiles; 512 x 256 is the wrong shape; 128
  // is smaller than the original 256.
  REQUIRE(
      gfx::save_png(gfx::HdImage(500, 500, gfx::pack_rgba(9, 9, 9, 255)),
                    dir.path() / "locations" / "grid" / "gfx" / "landsym.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(512, 256, gfx::pack_rgba(9, 9, 9, 255)),
                        dir.path() / "locations" / "shape" / "gfx" /
                            "landsym.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(128, 128, gfx::pack_rgba(9, 9, 9, 255)),
                        dir.path() / "locations" / "small" / "gfx" /
                            "landsym.png"));
  resources::HdPack::instance().configure(dir.path());
  const gfx::Surface landsym(512, 512, gfx::PaletteHandle{0}, 7);
  for (const char* ns : {"grid", "shape", "small"}) {
    INFO(ns);
    const ArtScope scope{.ns = ns, .district = ""};
    CHECK(&views::hd::location_picture(landsym, "gfx/landsym", scope) ==
          &landsym);
    CHECK(&views::hd::location_picture(landsym, "gfx/landsym", scope) ==
          &landsym);
  }
  CHECK(lines.size() == 3); // one line each, not one per call

  // No HD at all: the classic picture, silently.
  resources::HdPack::instance().configure({});
  views::hd::clear_location_pictures();
  lines.clear();
  CHECK(&views::hd::location_picture(landsym, "gfx/landsym",
                                     ArtScope{.ns = "grid", .district = ""}) ==
        &landsym);
  CHECK(lines.empty());
}

TEST_CASE("the North Pole far map draws its namespaced atlas from the pack",
          "[hd][location][game]") {
  const auto game = test_support::game_path();
  if (!game) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the original game's folder");
  }
  Clean clean;
  ScopedHdScale scale(2);
  const std::string saved_path = fileutils::game_data_path();
  fileutils::game_data_path(game->string());
  VgaManager vga{VgaLoader()};
  const gfx::Surface& classic = vga.get("gfx/landsym");
  REQUIRE(classic.width() == 512);
  REQUIRE(classic.height() == 512);

  // Classic: the 1994 picture itself.
  CHECK(&views::hd::city_atlas(vga, Roster::classic(), CityId{0},
                               CityAtlas::Tiles) == &classic);

  test_support::TempDir dir;
  const uint32_t aurora = gfx::pack_rgba(40, 200, 150, 255);
  REQUIRE(gfx::save_png(gfx::HdImage(1024, 1024, aurora),
                        dir.path() / "locations" / "north-pole" / "gfx" /
                            "landsym.png"));
  resources::HdPack::instance().configure(dir.path());
  const Roster roster = north_pole_roster();
  REQUIRE(views::hd::atlas_key(roster, CityId{0}, CityAtlas::Tiles) ==
          "north-pole:gfx/landsym");
  const gfx::Surface& tiles =
      views::hd::city_atlas(vga, roster, CityId{0}, CityAtlas::Tiles);
  CHECK(&tiles != &classic);
  CHECK(first_texel(tiles) == aurora);
  CHECK(std::memcmp(tiles.data(), classic.data(),
                    static_cast<std::size_t>(512 * 512)) == 0);
  // The medium atlas has no location art: the classic one.
  CHECK(&views::hd::city_atlas(vga, roster, CityId{0}, CityAtlas::Medium) ==
        &vga.get("gfx/med1"));
  fileutils::game_data_path(saved_path);
}
