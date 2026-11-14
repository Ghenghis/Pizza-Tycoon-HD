/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <vector>
#include "engine/Lighting.h"

// Day and night, for the picture only: the game clock's time of day turned
// into a tint (through the grade), a night factor (for emissive glow and
// street lamps) and the sun's direction (for normal-map lighting). Nothing
// here reaches the game.
namespace fx {

struct DayNight {
  float exposure = 1.0f; // multiplies the grade's exposure
  float warmth = 0.0f;   // added to the grade's warmth
  float night = 0.0f;    // 0 by day .. 1 at night
  bool operator==(const DayNight&) const = default;
};

// The curve through the day, smooth everywhere and periodic over 24 hours:
// night until 5:00, dawn (warm, brightening) to 8:00, day 8:00-17:00
// (exactly neutral), dusk (amber, dimming) to 20:30, night (dim and blue)
// from there. `hours` is 0..24 (wrapped otherwise).
[[nodiscard]] DayNight day_night(float hours);

// The sun for normal-map lighting: a direction towards the sun (x right, y
// up the screen, z out of it), its colour times its strength (0 at night).
struct Sun {
  float x = 0.0f;
  float y = 0.0f;
  float z = 1.0f;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};
[[nodiscard]] Sun sun_at(float hours);

// A street lamp on the city map, in engine pixels of the map's layer.
struct LampSpot {
  float x = 0.0f;
  float y = 0.0f;
};

// Lamps along the roads of a tile map: on road tiles that border a non-road
// tile (the kerb), on a diagonal lattice `spacing` tiles apart, so they
// spread evenly without lining up. Deterministic: the same map always gets
// the same lamps. `origin` is where tile (0, 0) sits in the layer.
[[nodiscard]] std::vector<LampSpot>
place_street_lamps(int columns, int rows, float tile_px,
                   const std::function<bool(int, int)>& is_road,
                   int spacing = 4, LampSpot origin = {});

// The city map's lighting at night: the ambient level falls with `night`
// (to 0.55 at full night, tinted blue) and each lamp brings a warm sodium
// pool back up, with a small glow on the lamp itself. night 0 is the unlit
// map (ambient 1, no lights): Scene::active() is false.
[[nodiscard]] lighting::Scene night_scene(float night,
                                          const std::vector<LampSpot>& lamps,
                                          float lamp_radius = 10.0f);

} // namespace fx
