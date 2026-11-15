/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The HD far map's tile classes follow the tile byte's low nibble (its
// landsym row, City::tile_description) and the game's own road test
// (City::is_road_tile). The old City::tile_category() read the high nibble,
// which put road smoothing on meadows and water smoothing on streets; it had
// no other user and is gone.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/city/City.h"
#include "views/hd/CityMapHd.h"

using views::hd::far_map_tile_class;
using views::hd::MapTileClass;

TEST_CASE("far map tile classes come from the landsym row", "[hd][city]") {
  // The high nibble only picks the picture within a row.
  CHECK(far_map_tile_class(0x60, false) == MapTileClass::Ground); // nature
  CHECK(far_map_tile_class(0x70, false) == MapTileClass::Ground);
  CHECK(far_map_tile_class(0x06, true) == MapTileClass::Road);
  CHECK(far_map_tile_class(0x47, true) == MapTileClass::Road);
  // Rows 6 and 7 that the game does not count as road.
  CHECK(far_map_tile_class(0xF6, false) == MapTileClass::Ground);
  CHECK(far_map_tile_class(0x05, false) == MapTileClass::Road); // path
  CHECK(far_map_tile_class(0x23, false) == MapTileClass::Water);
  CHECK(far_map_tile_class(0x34, false) == MapTileClass::Building);
  CHECK(far_map_tile_class(0x0B, false) == MapTileClass::Building);
  CHECK(far_map_tile_class(0x5A, false) == MapTileClass::Building); // shop
  CHECK(far_map_tile_class(0x98, false) == MapTileClass::Building);
  CHECK(far_map_tile_class(0x18, false) == MapTileClass::Ground);
  CHECK(far_map_tile_class(0x31, false) == MapTileClass::Ground);
}

TEST_CASE("far map tile classes on the real cities", "[hd][city][game]") {
  const auto root = test_support::game_path();
  if (!root) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the original game's folder");
  }
  int checked = 0;
  for (int id = 0; id < 10; ++id) {
    const std::string n = std::to_string(id);
    std::ifstream in(*root / "DAT" / ("MAP" + n + ".DAT"), std::ios::binary);
    if (!in) {
      in.clear();
      in.open(*root / "dat" / ("map" + n + ".dat"), std::ios::binary);
    }
    if (!in) {
      continue;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() == 160u * 120u);
    City city(core::CityId{id});
    city.map_data(bytes);
    city.data_length(static_cast<int>(bytes.size()));
    INFO("city " << id);
    int roads = 0;
    for (int i = 0; i < city.data_length(); ++i) {
      const uint8_t t = bytes[static_cast<std::size_t>(i)];
      // City::type() looks up the owner of a shop premise, and this city has
      // no premises loaded; premises are never roads anyway.
      const bool premise = City::is_business_premise_by_type(t);
      const bool road = !premise && city.is_road_tile(i);
      const MapTileClass cls = far_map_tile_class(t, road);
      if (premise) {
        REQUIRE(cls == MapTileClass::Building);
      }
      if (road) {
        REQUIRE(cls == MapTileClass::Road);
        ++roads;
      }
      if ((t & 0x0F) == 0x0) { // nature: meadows, trees, parks
        REQUIRE(cls == MapTileClass::Ground);
        REQUIRE_FALSE(city.is_road_tile(i));
      }
      if ((t & 0x0F) == 0x3) {
        REQUIRE(cls == MapTileClass::Water);
      }
    }
    CHECK(roads > 1000); // every city has a street grid
    ++checked;
  }
  CHECK(checked > 0);
}
