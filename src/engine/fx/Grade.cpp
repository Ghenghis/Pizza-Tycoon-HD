/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/Grade.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr float pi = 3.14159265f;

float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

float smoothstep(float e0, float e1, float x) {
  const float t = saturate((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}

float frac(float v) { return v - std::floor(v); }

Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb operator*(Rgb a, float s) { return {a.r * s, a.g * s, a.b * s}; }

RgbImage halve(const RgbImage& in) {
  RgbImage out;
  out.w = std::max(1, in.w / 2);
  out.h = std::max(1, in.h / 2);
  out.px.resize(static_cast<std::size_t>(out.w) *
                static_cast<std::size_t>(out.h));
  for (int y = 0; y < out.h; ++y) {
    const int y0 = std::min(in.h - 1, y * 2);
    const int y1 = std::min(in.h - 1, y * 2 + 1);
    for (int x = 0; x < out.w; ++x) {
      const int x0 = std::min(in.w - 1, x * 2);
      const int x1 = std::min(in.w - 1, x * 2 + 1);
      out.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.w) +
             static_cast<std::size_t>(x)] =
          (in.at(x0, y0) + in.at(x1, y0) + in.at(x0, y1) + in.at(x1, y1)) *
          0.25f;
    }
  }
  return out;
}

uint32_t channel(float v) {
  return static_cast<uint32_t>(std::lround(saturate(v) * 255.0f));
}

} // namespace

Rgb sample_bilinear(const RgbImage& img, float u, float v) {
  const float x = u * static_cast<float>(img.w) - 0.5f;
  const float y = v * static_cast<float>(img.h) - 0.5f;
  const float fx0 = std::floor(x);
  const float fy0 = std::floor(y);
  const float tx = x - fx0;
  const float ty = y - fy0;
  auto clamp_x = [&img](int i) { return std::clamp(i, 0, img.w - 1); };
  auto clamp_y = [&img](int i) { return std::clamp(i, 0, img.h - 1); };
  const int x0 = clamp_x(static_cast<int>(fx0));
  const int x1 = clamp_x(static_cast<int>(fx0) + 1);
  const int y0 = clamp_y(static_cast<int>(fy0));
  const int y1 = clamp_y(static_cast<int>(fy0) + 1);
  const Rgb top = img.at(x0, y0) * (1.0f - tx) + img.at(x1, y0) * tx;
  const Rgb bottom = img.at(x0, y1) * (1.0f - tx) + img.at(x1, y1) * tx;
  return top * (1.0f - ty) + bottom * ty;
}

RgbImage bloom_source(const RgbImage& frame) {
  return halve(halve(halve(frame)));
}

Rgb bloom_light(const RgbImage& source, float u, float v) {
  const float tx = 1.0f / static_cast<float>(source.w);
  const float ty = 1.0f / static_cast<float>(source.h);
  auto s = [&](float du, float dv) {
    return sample_bilinear(source, u + du, v + dv);
  };
  Rgb sum = s(0, 0) * 4.0f;
  sum = sum + s(tx, 0) * 2.0f + s(-tx, 0) * 2.0f + s(0, ty) * 2.0f +
        s(0, -ty) * 2.0f;
  sum = sum + s(tx, ty) + s(-tx, -ty) + s(tx, -ty) + s(-tx, ty);
  return sum * (1.0f / 16.0f);
}

Rgb grade_texel(const Grade& g, Rgb c, Rgb glow, float u, float v,
                const FrameInfo& info) {
  if (u < info.split_u) {
    return c;
  }
  if (g.bloom > 0.0f) {
    const float level = std::max({glow.r, glow.g, glow.b});
    const float over = saturate((level - g.bloom_threshold) /
                                std::max(1.0f - g.bloom_threshold, 1e-4f));
    c = c + glow * (over * over * g.bloom);
  }
  const std::array<float, 3> m = g.modulation();
  c = {c.r * m[0], c.g * m[1], c.b * m[2]};
  c = {(c.r - 0.5f) * g.contrast + 0.5f, (c.g - 0.5f) * g.contrast + 0.5f,
       (c.b - 0.5f) * g.contrast + 0.5f};
  const float l = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
  c = {l + (c.r - l) * g.saturation, l + (c.g - l) * g.saturation,
       l + (c.b - l) * g.saturation};
  float ch[3] = {c.r, c.g, c.b};
  for (int i = 0; i < 3; ++i) {
    const auto k = static_cast<std::size_t>(i);
    float x = g.gain[k] * (ch[i] + g.lift[k] * (1.0f - ch[i]));
    ch[i] = std::pow(std::max(x, 0.0f), 1.0f / g.gamma[k]);
  }
  c = {ch[0], ch[1], ch[2]};
  if (g.vignette > 0.0f) {
    const float du = u - 0.5f;
    const float dv = v - 0.5f;
    const float r = std::sqrt(du * du + dv * dv) * 1.41421356f;
    c = c * (1.0f - g.vignette * smoothstep(0.45f, 1.0f, r));
  }
  if (g.crt > 0.0f) {
    const float row = frac(v * info.scan_rows);
    const float scan = 0.5f + 0.5f * std::cos(2.0f * pi * (row - 0.5f));
    c = c * (1.0f + (0.72f + 0.28f * scan - 1.0f) * g.crt);
    const float col = frac(u * info.mask_cols / 3.0f) * 3.0f;
    const Rgb mask = col < 1.0f   ? Rgb{1.0f, 0.82f, 0.82f}
                     : col < 2.0f ? Rgb{0.82f, 1.0f, 0.82f}
                                  : Rgb{0.82f, 0.82f, 1.0f};
    const float t = g.crt * 0.5f;
    c = {c.r * (1.0f + (mask.r * 1.12f - 1.0f) * t),
         c.g * (1.0f + (mask.g * 1.12f - 1.0f) * t),
         c.b * (1.0f + (mask.b * 1.12f - 1.0f) * t)};
  }
  return {saturate(c.r), saturate(c.g), saturate(c.b)};
}

RgbImage from_rgba8888(const uint32_t* px, int w, int h, int pitch_px) {
  RgbImage img;
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint32_t t =
          px[static_cast<std::size_t>(y) * static_cast<std::size_t>(pitch_px) +
             static_cast<std::size_t>(x)];
      img.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
             static_cast<std::size_t>(x)] = {
          static_cast<float>((t >> 24) & 0xFF) / 255.0f,
          static_cast<float>((t >> 16) & 0xFF) / 255.0f,
          static_cast<float>((t >> 8) & 0xFF) / 255.0f};
    }
  }
  return img;
}

void grade_frame(const Grade& g, const uint32_t* in, uint32_t* out, int w,
                 int h, const FrameInfo& info) {
  const auto count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  if (g.picture_identity()) {
    std::memcpy(out, in, count * sizeof(uint32_t));
    return;
  }
  const RgbImage frame = from_rgba8888(in, w, h, w);
  RgbImage glow_source;
  if (g.bloom > 0.0f) {
    glow_source = bloom_source(frame);
  }
  for (int y = 0; y < h; ++y) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const std::size_t i =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
          static_cast<std::size_t>(x);
      if (u < info.split_u) {
        out[i] = in[i];
        continue;
      }
      const Rgb glow =
          g.bloom > 0.0f ? bloom_light(glow_source, u, v) : Rgb{0, 0, 0};
      const Rgb c = grade_texel(g, frame.px[i], glow, u, v, info);
      out[i] = (channel(c.r) << 24) | (channel(c.g) << 16) |
               (channel(c.b) << 8) | (in[i] & 0xFF);
    }
  }
}

} // namespace fx
