/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <cstdint>
#include <vector>

// The North Pole's ambient effects, drawn in the existing SDL pipeline:
//   * an aurora band over the city map at night, a slow, low gradient drift
//     drawn additively (so the same whether before or after the glow);
//   * a twinkle on the emissive glow, per cluster of 8 engine pixels, up to
//     +-15%, smooth in time;
//   * (snow is fx::Snowfall; the music accent is in fx::resolve_frame.)
// Everything is a pure function of the animation time and the position: the
// same game time draws the same frame, and nothing here has any state the
// game or its random numbers could see.
namespace fx {

inline constexpr float twinkle_depth = 0.15f;
inline constexpr int twinkle_cluster_px = 8; // engine pixels

// 1 +- twinkle_depth for cluster (cx, cy) at time t, changing smoothly a
// little over once a second. Deterministic.
[[nodiscard]] float twinkle(int cx, int cy, double t);

// The aurora over `rect` (target texels) at time t with `strength` 0..1:
// a strip of quads, two triangles each, coloured straight (r, g, b, a) for
// SDL_BLENDMODE_ADD, transparent at its top and bottom edges. Replaces the
// vectors' contents; empty for strength 0.
void aurora_mesh(const SDL_FRect& rect, double t, float strength,
                 std::vector<SDL_Vertex>& vertices, std::vector<int>& indices);

// The glow texture drawn over `dest` (target texels) as a grid of clusters
// (`cell` texels each), vertex colour strength * twinkle(), clamped to 1:
// with the glow's premultiplied ADD this flickers each cluster. The texture
// coordinates cover the whole texture.
void twinkle_mesh(const SDL_FRect& dest, float strength, double t, float cell,
                  std::vector<SDL_Vertex>& vertices, std::vector<int>& indices);

} // namespace fx
