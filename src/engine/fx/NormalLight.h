/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <array>
#include <unordered_map>
#include "engine/Lighting.h"
#include "engine/fx/DayNight.h"
#include "engine/fx/Grade.h"
#include "engine/gfx/HdImage.h"

// Relief lighting from normal maps (shaders/normal_light.frag.hlsl): a
// picture with a normal map (fx::MapStore) is shaped by the lights around
// it, while its overall brightness stays what the light pass gives it. The
// factor per texel is light(N) / light(flat), so a flat map changes nothing
// and a picture without a map is drawn as before.
namespace fx {

inline constexpr int max_normal_lights = 8;

// A point light in the target's texels (y down).
struct PointLight {
  float x = 0.0f;
  float y = 0.0f;
  float height = 0.0f; // above the picture, texels
  float radius = 0.0f; // where it fades out, texels
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

struct NormalLights {
  Rgb ambient{0.35f, 0.35f, 0.35f};
  Sun sun;
  int count = 0;
  std::array<PointLight, max_normal_lights> points{};
  // True when nothing would be shaped (no sun and no point light).
  [[nodiscard]] bool empty() const;
};

// The lights of a layer's lighting::Scene at `seconds` (the brightest
// eight), scaled to texels (k per engine pixel), plus the sun.
[[nodiscard]] NormalLights normal_lights(const lighting::Scene& scene, int k,
                                         double seconds, const Sun& sun);

// A normal map texel (straight alpha, 0xRRGGBBAA) as a unit vector: R = +X,
// G = +Y (up), B = +Z; 128 is 0, so (128, 128, 255) is exactly flat;
// transparent texels are flat.
struct Normal {
  float x = 0.0f;
  float y = 0.0f;
  float z = 1.0f;
};
[[nodiscard]] Normal decode_normal(uint32_t texel);

// CPU reference of the shader: the factor at target texel (px, py), 0..2
// per channel (1 everywhere for a flat normal).
[[nodiscard]] Rgb relief(const NormalLights& lights, Normal n, float px,
                         float py);
// Shades a straight-alpha picture drawn at (dx, dy) of the target with its
// normal map (same size), as the shader does; alpha kept.
[[nodiscard]] gfx::HdImage shade(const NormalLights& lights,
                                 const gfx::HdImage& picture,
                                 const gfx::HdImage& normals, int dx, int dy);

// The shader's uniforms, 20 float4s.
struct NormalLightUniforms {
  std::array<float, 4> ambient{};
  std::array<float, 4> sun_dir{};
  std::array<float, 4> sun_colour{};
  std::array<float, 4> count{};
  std::array<std::array<float, 4>, max_normal_lights> light_pos{};
  std::array<std::array<float, 4>, max_normal_lights> light_colour{};
};
static_assert(sizeof(NormalLightUniforms) == 320);
[[nodiscard]] NormalLightUniforms make_uniforms(const NormalLights& lights);

// The GPU side, on SDL's GPU render driver only: one render state per
// normal-map texture (its sampler binding is fixed when the state is made).
class NormalLightPass {
public:
  explicit NormalLightPass(SDL_Renderer* renderer);
  ~NormalLightPass();
  NormalLightPass(const NormalLightPass&) = delete;
  NormalLightPass& operator=(const NormalLightPass&) = delete;

  // True on SDL's GPU driver once the shader loaded (tried once).
  [[nodiscard]] bool ready();
  // Draws picture[src] at dst of the current target through the shader.
  // False, drawing nothing, when the shader or the normal map's GPU
  // texture is not there (the caller then draws the picture as usual).
  bool draw(SDL_Texture* picture, SDL_Texture* normals, const SDL_FRect& src,
            const SDL_FRect& dst, const NormalLights& lights);
  // Forgets the render states (their textures may be gone).
  void reset();

private:
  SDL_Renderer* renderer_;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUShader* shader_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  bool failed_ = false;
  std::unordered_map<SDL_GPUTexture*, SDL_GPURenderState*> states_;
};

} // namespace fx
