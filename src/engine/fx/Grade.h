/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <vector>
#include "engine/fx/Look.h"

// The CPU reference of the post-FX pass: the same maths as
// shaders/postfx.frag.hlsl, step for step, so the look can be tested (and
// its presets compared) without a GPU. Change both together.
namespace fx {

struct Rgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

// What the pass needs to know about the frame besides the grade.
struct FrameInfo {
  // Texture u left of which the frame stays as it is (the 1994 side of the
  // F8 comparison); 0 = graded everywhere.
  float split_u = 0.0f;
  // Rows of the original picture (the CRT scanlines follow them): 400.
  float scan_rows = 400.0f;
  // Columns of the output (the CRT mask follows the screen's pixels).
  float mask_cols = 640.0f;
};

// An RGB picture in floats (0..1, sRGB-encoded).
struct RgbImage {
  int w = 0;
  int h = 0;
  std::vector<Rgb> px;
  [[nodiscard]] const Rgb& at(int x, int y) const {
    return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
              static_cast<std::size_t>(x)];
  }
};

// Bilinear sample at (u, v), clamped at the edges, as a GPU sampler does.
[[nodiscard]] Rgb sample_bilinear(const RgbImage& img, float u, float v);

// The bloom source: the frame reduced three times by 2x2 boxes (1/8 size),
// as PostFx does with linear-filtered halvings.
[[nodiscard]] RgbImage bloom_source(const RgbImage& frame);
// The blurred light at (u, v): a 9-tap tent over the bloom source.
[[nodiscard]] Rgb bloom_light(const RgbImage& source, float u, float v);

// One texel through the pass. `glow` is bloom_light() at the texel (unused
// when the grade has no bloom).
[[nodiscard]] Rgb grade_texel(const Grade& g, Rgb c, Rgb glow, float u, float v,
                              const FrameInfo& info);

// Packing of SDL_PIXELFORMAT_RGBA8888: 0xRRGGBBAA.
[[nodiscard]] RgbImage from_rgba8888(const uint32_t* px, int w, int h,
                                     int pitch_px);
// The whole pass over an RGBA8888 frame, alpha kept. `in` and `out` hold
// w * h texels (pitch w). The identity grade copies `in` exactly.
void grade_frame(const Grade& g, const uint32_t* in, uint32_t* out, int w,
                 int h, const FrameInfo& info);

} // namespace fx
