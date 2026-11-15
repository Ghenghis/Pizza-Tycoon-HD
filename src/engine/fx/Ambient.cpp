/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/Ambient.h"

#include <algorithm>
#include <cmath>

namespace fx {

namespace {

// -1..1 from a cluster and a step, the same everywhere for the same input.
float hashed(int x, int y, int64_t n) {
  uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u;
  h ^= static_cast<uint32_t>(y) * 0x85EBCA77u;
  h ^= static_cast<uint32_t>(n) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

void quad(std::vector<int>& indices, int a, int b, int c, int d) {
  for (int v : {a, b, c, a, c, d}) {
    indices.push_back(v);
  }
}

} // namespace

float twinkle(int cx, int cy, double t) {
  const double steps = std::max(0.0, t) * 1.3;
  const double f = std::floor(steps);
  const auto n = static_cast<int64_t>(f);
  const float s = smooth(static_cast<float>(steps - f));
  const float a = hashed(cx, cy, n);
  const float b = hashed(cx, cy, n + 1);
  return 1.0f + twinkle_depth * (a + (b - a) * s);
}

void aurora_mesh(const SDL_FRect& rect, double t, float strength,
                 std::vector<SDL_Vertex>& vertices, std::vector<int>& indices) {
  vertices.clear();
  indices.clear();
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (strength <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  constexpr int columns = 32;
  const auto ft = static_cast<float>(std::fmod(t, 3600.0));
  for (int i = 0; i <= columns; ++i) {
    const float u = static_cast<float>(i) / columns;
    const float x = rect.x + u * rect.w;
    // A slow, low drift of the band's height and thickness.
    const float centre =
        rect.y + rect.h * (0.22f + 0.04f * std::sin(ft * 0.07f + u * 5.0f));
    const float half =
        rect.h * 0.09f * (0.8f + 0.2f * std::sin(ft * 0.05f + u * 9.0f + 1.0f));
    // Green into violet along the band, the mix wandering slowly.
    const float mix = 0.5f + 0.5f * std::sin(ft * 0.03f + u * 4.0f);
    const SDL_FColor core{0.20f + 0.40f * mix, 1.0f - 0.60f * mix,
                          0.55f + 0.45f * mix, 0.22f * strength};
    const SDL_FColor edge{core.r, core.g, core.b, 0.0f};
    vertices.push_back({{x, centre - half}, edge, {0, 0}});
    vertices.push_back({{x, centre}, core, {0, 0}});
    vertices.push_back({{x, centre + half * 1.6f}, edge, {0, 0}});
  }
  for (int i = 0; i < columns; ++i) {
    const int a = i * 3;
    const int b = (i + 1) * 3;
    quad(indices, a, b, b + 1, a + 1);
    quad(indices, a + 1, b + 1, b + 2, a + 2);
  }
}

void twinkle_mesh(const SDL_FRect& dest, float strength, double t, float cell,
                  std::vector<SDL_Vertex>& vertices,
                  std::vector<int>& indices) {
  vertices.clear();
  indices.clear();
  if (dest.w <= 0.0f || dest.h <= 0.0f || cell <= 0.0f) {
    return;
  }
  const int cols = std::max(1, static_cast<int>(std::ceil(dest.w / cell)));
  const int rows = std::max(1, static_cast<int>(std::ceil(dest.h / cell)));
  for (int j = 0; j <= rows; ++j) {
    const float y = std::min(dest.h, static_cast<float>(j) * cell);
    for (int i = 0; i <= cols; ++i) {
      const float x = std::min(dest.w, static_cast<float>(i) * cell);
      const float f = std::min(1.0f, strength * twinkle(i, j, t));
      vertices.push_back({{dest.x + x, dest.y + y},
                          {f, f, f, 1.0f},
                          {x / dest.w, y / dest.h}});
    }
  }
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < cols; ++i) {
      const int a = j * (cols + 1) + i;
      quad(indices, a, a + 1, a + cols + 2, a + cols + 1);
    }
  }
}

} // namespace fx
