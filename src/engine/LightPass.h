/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include "engine/Lighting.h"

// Lights one layer on the GPU, with fixed-function blending only (works on
// every SDL renderer):
//   1. a light map at engine resolution: the ambient colour, plus a soft
//      radial falloff per light, added up;
//   2. the layer multiplied by that map (smoothly scaled up), so the room
//      sits at the ambient level and each light brings its pool back up;
//   3. an additive halo on emitters (flames, screens) for a gentle glow.
class LightPass {
public:
  explicit LightPass(SDL_Renderer* renderer);
  ~LightPass();
  LightPass(const LightPass&) = delete;
  LightPass& operator=(const LightPass&) = delete;

  // out = albedo (premultiplied, w*k x h*k) lit by `scene` at time
  // `seconds`. w and h are the layer's size in engine pixels. Leaves the
  // render target set to `out`.
  void apply(SDL_Texture* albedo, SDL_Texture* out, int w, int h, int k,
             const lighting::Scene& scene, double seconds);

private:
  SDL_Texture* gradient();
  SDL_Texture* light_map(int w, int h);

  SDL_Renderer* renderer_;
  SDL_Texture* gradient_ = nullptr;
  SDL_Texture* map_ = nullptr;
  int map_w_ = 0;
  int map_h_ = 0;
};
