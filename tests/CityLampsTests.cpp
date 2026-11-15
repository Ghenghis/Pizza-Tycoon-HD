/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The city map's street lamps (CityView::lighting through
// views/screens/city_view/CityLamps.h): no light by day or with "Day and
// night" off, so the map is drawn exactly as before; at night a lamp on the
// kerbs of the roads, placed where the map is drawn this frame.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "engine/fx/DayNight.h"
#include "engine/fx/Look.h"
#include "engine/geom/Spaces.h"
#include "views/screens/city_view/CityLamps.h"

using views::city_lamps::city_scene;
using views::city_lamps::MapPlacement;
using views::city_lamps::visible_lamps;

namespace {

constexpr int columns = 40;
constexpr int rows = 30;

// A cross of two-lane roads through the middle of the map.
std::vector<uint8_t> cross_roads() {
  std::vector<uint8_t> roads(columns * rows, 0);
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < columns; ++i) {
      if (i == 19 || i == 20 || j == 14 || j == 15) {
        roads[static_cast<std::size_t>((j * columns) + i)] = 1;
      }
    }
  }
  return roads;
}

const MapPlacement close_map{.tile_px = 32.0f,
                             .origin = {.x = -200.0f, .y = -150.0f},
                             .visible = ViewRect{0, 0, 640, 400}};

float night_at(bool day_night, fx::Preset preset, float hours) {
  fx::LookSettings settings;
  settings.preset = preset;
  settings.day_night = day_night;
  return fx::resolve_frame(
             settings, fx::FrameContext{.area = fx::Area::City, .hours = hours})
      .night;
}

} // namespace

TEST_CASE("City lamps: no light by day or with day and night off",
          "[fx][lighting][city]") {
  const auto roads = cross_roads();
  // By day.
  const float noon = night_at(true, fx::Preset::Natural, 12.0f);
  CHECK(noon == 0.0f);
  CHECK_FALSE(city_scene(noon, roads, columns, rows, close_map).active());
  // "Day and night" off, at midnight.
  const float off = night_at(false, fx::Preset::Natural, 0.0f);
  CHECK(off == 0.0f);
  CHECK_FALSE(city_scene(off, roads, columns, rows, close_map).active());
  // The Classic preset never has night either.
  const float classic = night_at(true, fx::Preset::Classic, 0.0f);
  CHECK(classic == 0.0f);
  CHECK_FALSE(city_scene(classic, roads, columns, rows, close_map).active());
}

TEST_CASE("City lamps: at night the roads are lit where the map is drawn",
          "[fx][lighting][city]") {
  const auto roads = cross_roads();
  const float midnight = night_at(true, fx::Preset::Natural, 0.0f);
  REQUIRE(midnight > 0.0f);
  const auto scene = city_scene(midnight, roads, columns, rows, close_map);
  CHECK(scene.active());
  CHECK(scene.ambient < 1.0f);
  REQUIRE_FALSE(scene.lights.empty());

  // The lamps are fx::place_street_lamps' for this placement, those that
  // reach the visible map.
  const float radius = views::city_lamps::lamp_radius(close_map.tile_px);
  const auto lamps = visible_lamps(roads, columns, rows, close_map, radius);
  REQUIRE(lamps.size() == scene.lights.size());
  for (std::size_t k = 0; k < lamps.size(); ++k) {
    CHECK(scene.lights[k].x == lamps[k].x);
    CHECK(scene.lights[k].y == lamps[k].y);
    CHECK(lamps[k].x >= -radius);
    CHECK(lamps[k].x <= 640.0f + radius);
    CHECK(lamps[k].y >= -radius);
    CHECK(lamps[k].y <= 400.0f + radius);
  }
  const auto all = fx::place_street_lamps(
      columns, rows, close_map.tile_px,
      [&](int i, int j) {
        return roads[static_cast<std::size_t>((j * columns) + i)] != 0;
      },
      4, close_map.origin);
  CHECK(lamps.size() < all.size()); // some lie off the visible map

  // The map drawn 7.5 pixels further left and 3 up (a smoothed camera
  // between two steps): every lamp moves with it.
  MapPlacement moved = close_map;
  moved.origin.x -= 7.5f;
  moved.origin.y -= 3.0f;
  moved.visible = ViewRect{0, 0, 4000, 4000}; // keep them all
  MapPlacement still = close_map;
  still.visible = moved.visible;
  const auto before = visible_lamps(roads, columns, rows, still, radius);
  const auto after = visible_lamps(roads, columns, rows, moved, radius);
  REQUIRE(before.size() == after.size());
  for (std::size_t k = 0; k < before.size(); ++k) {
    CHECK(after[k].x == before[k].x - 7.5f);
    CHECK(after[k].y == before[k].y - 3.0f);
  }

  // A map with no roads shown (the far map, the district tabs) gets the
  // night's ambient light and no lamps.
  const auto bare = city_scene(midnight, {}, 0, 0, close_map);
  CHECK(bare.lights.empty());
}
