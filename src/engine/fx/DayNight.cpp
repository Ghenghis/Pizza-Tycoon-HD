/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/DayNight.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace fx {

namespace {

struct Key {
  float hour;
  DayNight at;
};

constexpr DayNight night_look{0.80f, -0.30f, 1.0f};
constexpr DayNight day_look{1.0f, 0.0f, 0.0f};

// One full day; the last key equals the first, so the curve wraps smoothly.
constexpr std::array keys{
    Key{0.0f, night_look},
    Key{5.0f, night_look},
    Key{6.5f, DayNight{0.92f, 0.20f, 0.45f}}, // dawn
    Key{8.0f, day_look},
    Key{17.0f, day_look},
    Key{18.75f, DayNight{0.90f, 0.30f, 0.35f}}, // dusk
    Key{20.5f, night_look},
    Key{24.0f, night_look},
};

constexpr float pi = 3.14159265f;

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float mix(float a, float b, float t) { return a == b ? a : a + (b - a) * t; }

} // namespace

DayNight day_night(float hours) {
  float h = std::fmod(hours, 24.0f);
  if (h < 0.0f) {
    h += 24.0f;
  }
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
    const Key& a = keys[i];
    const Key& b = keys[i + 1];
    if (h >= a.hour && h <= b.hour) {
      // Smoothstep between keys: no kink anywhere along the day.
      const float t = smooth((h - a.hour) / (b.hour - a.hour));
      if (t >= 1.0f) {
        return b.at;
      }
      return {mix(a.at.exposure, b.at.exposure, t),
              mix(a.at.warmth, b.at.warmth, t), mix(a.at.night, b.at.night, t)};
    }
  }
  return night_look;
}

Sun sun_at(float hours) {
  float h = std::fmod(hours, 24.0f);
  if (h < 0.0f) {
    h += 24.0f;
  }
  // Up at 6:00, highest at noon, down at 18:00; from the left in the
  // morning, from the right in the evening.
  const float angle = pi * (h - 6.0f) / 12.0f;
  const float elevation = std::sin(angle);
  Sun s;
  if (elevation <= 0.0f) {
    return s;
  }
  float x = -std::cos(angle);
  float y = 0.35f;
  float z = elevation;
  const float len = std::sqrt(x * x + y * y + z * z);
  s.x = x / len;
  s.y = y / len;
  s.z = z / len;
  const float strength = smooth(std::clamp(elevation / 0.2f, 0.0f, 1.0f));
  // Warmer near the horizon.
  s.r = strength;
  s.g = strength * (0.85f + 0.15f * elevation);
  s.b = strength * (0.70f + 0.30f * elevation);
  return s;
}

std::vector<LampSpot>
place_street_lamps(int columns, int rows, float tile_px,
                   const std::function<bool(int, int)>& is_road, int spacing,
                   LampSpot origin) {
  std::vector<LampSpot> lamps;
  spacing = std::max(1, spacing);
  auto road = [&](int i, int j) {
    return i >= 0 && j >= 0 && i < columns && j < rows && is_road(i, j);
  };
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < columns; ++i) {
      if ((i + 2 * j) % spacing != 0 || !road(i, j)) {
        continue;
      }
      const bool kerb = !road(i - 1, j) || !road(i + 1, j) || !road(i, j - 1) ||
                        !road(i, j + 1);
      if (kerb) {
        lamps.push_back({origin.x + (static_cast<float>(i) + 0.5f) * tile_px,
                         origin.y + (static_cast<float>(j) + 0.5f) * tile_px});
      }
    }
  }
  return lamps;
}

lighting::Scene night_scene(float night, const std::vector<LampSpot>& lamps,
                            float lamp_radius) {
  lighting::Scene scene;
  night = std::clamp(night, 0.0f, 1.0f);
  if (night <= 0.0f) {
    return scene;
  }
  scene.ambient = 1.0f - 0.45f * night;
  scene.ambient_r = static_cast<uint8_t>(std::lround(255.0f - 60.0f * night));
  scene.ambient_g = static_cast<uint8_t>(std::lround(255.0f - 40.0f * night));
  scene.ambient_b = 255;
  scene.lights.reserve(lamps.size());
  for (const LampSpot& spot : lamps) {
    lighting::Light l;
    l.x = spot.x;
    l.y = spot.y;
    l.radius = lamp_radius;
    // Sodium orange.
    l.r = 255;
    l.g = 196;
    l.b = 120;
    l.intensity = night;
    l.glow = 0.35f * night;
    l.glow_radius = std::max(1.0f, lamp_radius * 0.2f);
    scene.lights.push_back(l);
  }
  return scene;
}

} // namespace fx
