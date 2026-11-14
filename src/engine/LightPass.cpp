/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/LightPass.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "engine/Fatal.h"

namespace {

constexpr int gradient_size = 128;

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

LightPass::LightPass(SDL_Renderer* renderer) : renderer_(renderer) {}

LightPass::~LightPass() {
  if (gradient_) {
    SDL_DestroyTexture(gradient_);
  }
  if (map_) {
    SDL_DestroyTexture(map_);
  }
}

SDL_Texture* LightPass::gradient() {
  if (gradient_) {
    return gradient_;
  }
  // White, premultiplied, falling smoothly to 0 at the rim. (1 - d^2)^1.5:
  // broad enough that a light reads as a pool around its source, yet with no
  // visible edge.
  std::vector<uint32_t> px(static_cast<std::size_t>(gradient_size) *
                           gradient_size);
  const float c = (gradient_size - 1) / 2.0f;
  for (int y = 0; y < gradient_size; ++y) {
    for (int x = 0; x < gradient_size; ++x) {
      const float dx = (static_cast<float>(x) - c) / c;
      const float dy = (static_cast<float>(y) - c) / c;
      const float d2 = std::min(1.0f, dx * dx + dy * dy);
      const float f = std::pow(1.0f - d2, 1.5f);
      const auto v = static_cast<uint32_t>(std::lround(f * 255.0f));
      px[static_cast<std::size_t>(y) * gradient_size + x] =
          (v << 24) | (v << 16) | (v << 8) | v;
    }
  }
  gradient_ =
      SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_STATIC, gradient_size, gradient_size);
  if (!gradient_) {
    fatal(SDL_GetError());
  }
  SDL_UpdateTexture(gradient_, nullptr, px.data(), gradient_size * 4);
  SDL_SetTextureScaleMode(gradient_, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(gradient_, SDL_BLENDMODE_ADD);
  return gradient_;
}

SDL_Texture* LightPass::light_map(int w, int h) {
  if (map_ && map_w_ == w && map_h_ == h) {
    return map_;
  }
  if (map_) {
    SDL_DestroyTexture(map_);
  }
  map_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                           SDL_TEXTUREACCESS_TARGET, w, h);
  if (!map_) {
    fatal(SDL_GetError());
  }
  SDL_SetTextureScaleMode(map_, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(map_, SDL_BLENDMODE_MOD);
  map_w_ = w;
  map_h_ = h;
  return map_;
}

void LightPass::apply(SDL_Texture* albedo, SDL_Texture* out, int w, int h,
                      int k, const lighting::Scene& scene, double seconds) {
  SDL_Texture* grad = gradient();
  SDL_Texture* map = light_map(w, h);

  // 1. The light map (engine resolution is plenty: light is smooth).
  SDL_SetRenderTarget(renderer_, map);
  const float a = clamp01(scene.ambient);
  SDL_SetRenderDrawColorFloat(
      renderer_, a * static_cast<float>(scene.ambient_r) / 255.0f,
      a * static_cast<float>(scene.ambient_g) / 255.0f,
      a * static_cast<float>(scene.ambient_b) / 255.0f, 1.0f);
  SDL_RenderClear(renderer_);
  std::vector<lighting::Rgb> colours;
  colours.reserve(scene.lights.size());
  for (const lighting::Light& l : scene.lights) {
    const lighting::Rgb c = lighting::sample(l, seconds);
    colours.push_back(c);
    SDL_SetTextureColorModFloat(grad, clamp01(c.r), clamp01(c.g), clamp01(c.b));
    const SDL_FRect dst{l.x - l.radius, l.y - l.radius, l.radius * 2.0f,
                        l.radius * 2.0f};
    SDL_RenderTexture(renderer_, grad, nullptr, &dst);
  }

  // 2. The layer, multiplied by the light map.
  SDL_SetRenderTarget(renderer_, out);
  SDL_BlendMode albedo_mode = SDL_BLENDMODE_NONE;
  SDL_GetTextureBlendMode(albedo, &albedo_mode);
  SDL_SetTextureBlendMode(albedo, SDL_BLENDMODE_NONE);
  SDL_RenderTexture(renderer_, albedo, nullptr, nullptr);
  SDL_SetTextureBlendMode(albedo, albedo_mode);
  const SDL_FRect full{0, 0, static_cast<float>(w * k),
                       static_cast<float>(h * k)};
  SDL_RenderTexture(renderer_, map, nullptr, &full);

  // 3. Halos on the emitters themselves.
  const auto fk = static_cast<float>(k);
  for (std::size_t i = 0; i < scene.lights.size(); ++i) {
    const lighting::Light& l = scene.lights[i];
    if (l.glow <= 0.0f || l.glow_radius <= 0.0f) {
      continue;
    }
    const lighting::Rgb c = colours[i];
    SDL_SetTextureColorModFloat(grad, clamp01(c.r * l.glow),
                                clamp01(c.g * l.glow), clamp01(c.b * l.glow));
    const SDL_FRect dst{(l.x - l.glow_radius) * fk, (l.y - l.glow_radius) * fk,
                        l.glow_radius * 2.0f * fk, l.glow_radius * 2.0f * fk};
    SDL_RenderTexture(renderer_, grad, nullptr, &dst);
  }
  SDL_SetTextureColorModFloat(grad, 1.0f, 1.0f, 1.0f);
}
