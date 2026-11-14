/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/Lighting.h"

#include <algorithm>
#include <cmath>

namespace lighting {

namespace {

// Smooth value noise in 0..1, deterministic per seed.
float noise(double t, uint32_t seed) {
  auto hash = [seed](int64_t i) {
    uint32_t h = static_cast<uint32_t>(i) * 0x9E3779B1u ^ seed * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
  };
  const double f = std::floor(t);
  const auto i = static_cast<int64_t>(f);
  const auto u = static_cast<float>(t - f);
  const float s = u * u * (3.0f - 2.0f * u);
  return hash(i) + (hash(i + 1) - hash(i)) * s;
}

} // namespace

namespace {
bool g_enabled = true;
} // namespace

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

bool Scene::animated() const {
  return std::any_of(lights.begin(), lights.end(), [](const Light& l) {
    return l.flicker != Flicker::Steady;
  });
}

bool Scene::active() const { return ambient < 0.999f || !lights.empty(); }

Rgb sample(const Light& light, double seconds) {
  const uint32_t s = light.seed;
  float k = 1.0f;
  Rgb tint{1.0f, 1.0f, 1.0f};
  switch (light.flicker) {
  case Flicker::Steady:
    break;
  case Flicker::Candle: {
    k = 0.82f + 0.18f * noise(seconds * 9.0, s);
    // An occasional deeper dip, as a flame does in a draught.
    const float dip = noise(seconds * 1.3, s ^ 0xA5A5u);
    if (dip > 0.85f) {
      k *= 1.0f - (dip - 0.85f) * 1.6f;
    }
    break;
  }
  case Flicker::Fire:
    k = 0.75f + 0.15f * noise(seconds * 2.2, s) +
        0.10f * noise(seconds * 6.5, s + 7);
    break;
  case Flicker::Screen:
    k = 0.85f + 0.15f * noise(std::floor(seconds * 12.0), s);
    break;
  case Flicker::Cycle: {
    const double h = std::fmod(seconds * 0.15 + (s % 97) / 97.0, 1.0) * 6.0;
    const auto x = static_cast<float>(1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    switch (static_cast<int>(h)) {
    case 0:
      tint = {1, x, 0};
      break;
    case 1:
      tint = {x, 1, 0};
      break;
    case 2:
      tint = {0, 1, x};
      break;
    case 3:
      tint = {0, x, 1};
      break;
    case 4:
      tint = {x, 0, 1};
      break;
    default:
      tint = {1, 0, x};
      break;
    }
    // Keep some of the light's own colour so it never goes fully dark.
    tint = {0.4f + 0.6f * tint.r, 0.4f + 0.6f * tint.g, 0.4f + 0.6f * tint.b};
    break;
  }
  }
  const float a = std::clamp(k * light.intensity, 0.0f, 4.0f);
  return Rgb{static_cast<float>(light.r) / 255.0f * tint.r * a,
             static_cast<float>(light.g) / 255.0f * tint.g * a,
             static_cast<float>(light.b) / 255.0f * tint.b * a};
}

} // namespace lighting
