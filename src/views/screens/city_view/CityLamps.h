/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <vector>

#include "engine/Lighting.h"
#include "engine/fx/DayNight.h"
#include "engine/geom/Spaces.h"

// The city map's street lamps at night (the lighting lane's day and night,
// engine/fx/DayNight.h), for CityView::lighting. Drawing only.
namespace views::city_lamps {

// Where the map is drawn this frame, in pixels of the view's layer.
struct MapPlacement {
  float tile_px = 0.0f; // one map tile
  fx::LampSpot origin;  // where tile (0, 0) sits
  ViewRect visible;     // the part of the layer the map fills
};

// The map's lamps, placed by fx::place_street_lamps over a road mask (one
// byte per tile, row-major, nonzero for a road), keeping only those whose
// light reaches the visible part of the map.
[[nodiscard]] std::vector<fx::LampSpot>
visible_lamps(const std::vector<uint8_t>& roads, int columns, int rows,
              const MapPlacement& at, float lamp_radius);

// The lamp's light pool for a map tile of tile_px pixels.
[[nodiscard]] float lamp_radius(float tile_px);

// The scene for a frame: fx::night_scene(night, lamps). By day, and with
// "Day and night" off (night is then 0), it is inactive and the map is
// drawn exactly as before.
[[nodiscard]] lighting::Scene city_scene(float night,
                                         const std::vector<uint8_t>& roads,
                                         int columns, int rows,
                                         const MapPlacement& at);

} // namespace views::city_lamps
