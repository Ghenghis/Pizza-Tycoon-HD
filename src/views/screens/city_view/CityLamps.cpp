/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/city_view/CityLamps.h"

#include <algorithm>
#include <cstddef>

namespace views::city_lamps {

std::vector<fx::LampSpot> visible_lamps(const std::vector<uint8_t>& roads,
                                        int columns, int rows,
                                        const MapPlacement& at,
                                        float lamp_radius) {
  if (columns <= 0 || rows <= 0 || at.tile_px <= 0.0f ||
      roads.size() <
          static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows)) {
    return {};
  }
  std::vector<fx::LampSpot> lamps = fx::place_street_lamps(
      columns, rows, at.tile_px,
      [&](int i, int j) {
        return roads[(static_cast<std::size_t>(j) *
                      static_cast<std::size_t>(columns)) +
                     static_cast<std::size_t>(i)] != 0;
      },
      4, at.origin);
  const auto x0 = static_cast<float>(at.visible.x) - lamp_radius;
  const auto y0 = static_cast<float>(at.visible.y) - lamp_radius;
  const auto x1 = static_cast<float>(at.visible.x + at.visible.w) + lamp_radius;
  const auto y1 = static_cast<float>(at.visible.y + at.visible.h) + lamp_radius;
  std::erase_if(lamps, [&](const fx::LampSpot& l) {
    return l.x < x0 || l.x > x1 || l.y < y0 || l.y > y1;
  });
  return lamps;
}

float lamp_radius(float tile_px) {
  // The lighting lane's 10 pixels at the medium map's 8-pixel tiles, in
  // proportion at the close map's 32.
  return std::max(10.0f, 1.25f * tile_px);
}

lighting::Scene city_scene(float night, const std::vector<uint8_t>& roads,
                           int columns, int rows, const MapPlacement& at) {
  if (night <= 0.0f) {
    return fx::night_scene(0.0f, {}); // day, or "Day and night" off
  }
  const float radius = lamp_radius(at.tile_px);
  return fx::night_scene(night, visible_lamps(roads, columns, rows, at, radius),
                         radius);
}

} // namespace views::city_lamps
