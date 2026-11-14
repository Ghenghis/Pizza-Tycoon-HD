/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <vector>

// Dynamic 2D lighting for HD rendering: a screen reports its light sources
// (candles, ovens, arcade screens...) and an ambient level, and the renderer
// lights that layer on the GPU (LightPass). Only the picture changes; the
// game never sees any of this.
namespace lighting {

enum class Flicker : uint8_t {
  Steady,
  Candle, // quick small wavering, the odd dip
  Fire,   // slower, deeper glow of an oven or grill
  Screen, // TV and arcade screens: small fast changes
  Cycle,  // jukebox: colour slowly rotates
};

struct Light {
  float x = 0; // centre, engine pixels in the layer
  float y = 0;
  float radius = 0; // engine pixels to where the light fades out
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  float intensity = 1.0f;
  Flicker flicker = Flicker::Steady;
  uint32_t seed = 0;        // so neighbouring lights don't flicker in step
  float glow = 0.0f;        // additive halo on the light itself, 0..1
  float glow_radius = 0.0f; // engine pixels
};

struct Scene {
  // Light everywhere, before any light source: 1 = unlit picture as is.
  float ambient = 1.0f;
  uint8_t ambient_r = 255; // tint of the ambient light
  uint8_t ambient_g = 255;
  uint8_t ambient_b = 255;
  std::vector<Light> lights;

  // True when a light changes over time (the layer is relit every frame).
  [[nodiscard]] bool animated() const;
  // True when lighting would change the picture at all.
  [[nodiscard]] bool active() const;
};

struct Rgb {
  float r, g, b;
};

// The `lighting` option: off means no screen is lit (and the GPU picture
// matches the CPU one exactly). Set once per frame by the game loop.
void set_enabled(bool on);
[[nodiscard]] bool enabled();

// The colour a light gives at `seconds`, flicker included (0..1 per
// channel before intensity; intensity is applied).
[[nodiscard]] Rgb sample(const Light& light, double seconds);

} // namespace lighting
