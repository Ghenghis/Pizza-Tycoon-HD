/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/NormalLight.h"

#include <algorithm>
#include <cmath>
#include "engine/Logger.h"
#include "engine/fx/PostFx.h"
#include "engine/fx/generated/NormalLightShaders.h"

namespace fx {

bool NormalLights::empty() const {
  return count <= 0 && sun.r <= 0.0f && sun.g <= 0.0f && sun.b <= 0.0f;
}

NormalLights normal_lights(const lighting::Scene& scene, int k, double seconds,
                           const Sun& sun) {
  NormalLights out;
  const float a = std::clamp(scene.ambient, 0.0f, 1.0f);
  out.ambient = {a * static_cast<float>(scene.ambient_r) / 255.0f,
                 a * static_cast<float>(scene.ambient_g) / 255.0f,
                 a * static_cast<float>(scene.ambient_b) / 255.0f};
  out.sun = sun;
  struct Candidate {
    PointLight light;
    float weight;
  };
  std::vector<Candidate> all;
  all.reserve(scene.lights.size());
  const auto fk = static_cast<float>(k);
  for (const lighting::Light& l : scene.lights) {
    const lighting::Rgb c = lighting::sample(l, seconds);
    PointLight p;
    p.x = l.x * fk;
    p.y = l.y * fk;
    p.radius = l.radius * fk;
    // A light sits about half its reach above the picture.
    p.height = p.radius * 0.5f;
    p.r = c.r;
    p.g = c.g;
    p.b = c.b;
    all.push_back({p, (c.r + c.g + c.b) * l.radius});
  }
  std::stable_sort(all.begin(), all.end(),
                   [](const Candidate& x, const Candidate& y) {
                     return x.weight > y.weight;
                   });
  out.count =
      static_cast<int>(std::min<std::size_t>(all.size(), max_normal_lights));
  for (int i = 0; i < out.count; ++i) {
    out.points[static_cast<std::size_t>(i)] =
        all[static_cast<std::size_t>(i)].light;
  }
  return out;
}

Normal decode_normal(uint32_t texel) {
  if (gfx::alpha(texel) < 128) {
    return {};
  }
  auto axis = [](uint32_t c) {
    return std::clamp((static_cast<float>(c) - 128.0f) / 127.0f, -1.0f, 1.0f);
  };
  const float x = axis(gfx::red(texel));
  const float y = axis(gfx::green(texel));
  const float z = axis(gfx::blue(texel));
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len <= 1e-4f) {
    return {};
  }
  return {x / len, y / len, z / len};
}

namespace {

Rgb light_at(const NormalLights& lights, Normal n, float px, float py) {
  Rgb sum = lights.ambient;
  const float s = std::max(
      n.x * lights.sun.x + n.y * lights.sun.y + n.z * lights.sun.z, 0.0f);
  sum.r += lights.sun.r * s;
  sum.g += lights.sun.g * s;
  sum.b += lights.sun.b * s;
  for (int i = 0; i < std::min(lights.count, max_normal_lights); ++i) {
    const PointLight& p = lights.points[static_cast<std::size_t>(i)];
    const float dx = p.x - px;
    const float dy = py - p.y;
    const float d2 = (dx * dx + dy * dy) / std::max(p.radius * p.radius, 1e-4f);
    if (d2 >= 1.0f) {
      continue;
    }
    const float falloff = std::pow(1.0f - d2, 1.5f);
    const float len = std::sqrt(dx * dx + dy * dy + p.height * p.height);
    if (len <= 0.0f) {
      continue;
    }
    const float dot =
        std::max((n.x * dx + n.y * dy + n.z * p.height) / len, 0.0f);
    sum.r += p.r * falloff * dot;
    sum.g += p.g * falloff * dot;
    sum.b += p.b * falloff * dot;
  }
  return sum;
}

float ratio(float lit, float flat) {
  return flat > 1e-3f ? std::clamp(lit / flat, 0.0f, 2.0f) : 1.0f;
}

} // namespace

Rgb relief(const NormalLights& lights, Normal n, float px, float py) {
  const Rgb lit = light_at(lights, n, px, py);
  const Rgb flat = light_at(lights, Normal{}, px, py);
  return {ratio(lit.r, flat.r), ratio(lit.g, flat.g), ratio(lit.b, flat.b)};
}

gfx::HdImage shade(const NormalLights& lights, const gfx::HdImage& picture,
                   const gfx::HdImage& normals, int dx, int dy) {
  gfx::HdImage out = picture;
  auto channel = [](uint32_t c, float f) {
    return static_cast<uint32_t>(
        std::lround(std::clamp(static_cast<float>(c) * f, 0.0f, 255.0f)));
  };
  for (int y = 0; y < picture.height; ++y) {
    for (int x = 0; x < picture.width; ++x) {
      const uint32_t t = picture.at(x, y);
      const Rgb f = relief(lights, decode_normal(normals.at(x, y)),
                           static_cast<float>(dx + x) + 0.5f,
                           static_cast<float>(dy + y) + 0.5f);
      out.at(x, y) =
          gfx::pack_rgba(channel(gfx::red(t), f.r), channel(gfx::green(t), f.g),
                         channel(gfx::blue(t), f.b), gfx::alpha(t));
    }
  }
  return out;
}

NormalLightUniforms make_uniforms(const NormalLights& lights) {
  NormalLightUniforms u;
  u.ambient = {lights.ambient.r, lights.ambient.g, lights.ambient.b, 0.0f};
  u.sun_dir = {lights.sun.x, lights.sun.y, lights.sun.z, 0.0f};
  u.sun_colour = {lights.sun.r, lights.sun.g, lights.sun.b, 0.0f};
  const int count = std::clamp(lights.count, 0, max_normal_lights);
  u.count = {static_cast<float>(count), 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < count; ++i) {
    const auto j = static_cast<std::size_t>(i);
    const PointLight& p = lights.points[j];
    u.light_pos[j] = {p.x, p.y, p.height, p.radius};
    u.light_colour[j] = {p.r, p.g, p.b, 0.0f};
  }
  return u;
}

NormalLightPass::NormalLightPass(SDL_Renderer* renderer)
    : renderer_(renderer) {}

NormalLightPass::~NormalLightPass() {
  reset();
  if (device_) {
    if (sampler_) {
      SDL_ReleaseGPUSampler(device_, sampler_);
    }
    if (shader_) {
      SDL_ReleaseGPUShader(device_, shader_);
    }
  }
}

void NormalLightPass::reset() {
  for (auto& [tex, state] : states_) {
    SDL_DestroyGPURenderState(state);
  }
  states_.clear();
}

bool NormalLightPass::ready() {
  if (shader_) {
    return true;
  }
  if (failed_) {
    return false;
  }
  failed_ = true; // until it works
  if (!PostFx::is_gpu_driver(renderer_)) {
    return false;
  }
  device_ = SDL_GetGPURendererDevice(renderer_);
  if (!device_) {
    return false;
  }
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
  SDL_GPUShaderCreateInfo info{};
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 2;
  info.num_uniform_buffers = 1;
  if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
    info.code = shaders::normal_light_frag_dxil;
    info.code_size = sizeof(shaders::normal_light_frag_dxil);
    info.entrypoint = shaders::normal_light_entry;
  }
  else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = shaders::normal_light_frag_spirv;
    info.code_size = sizeof(shaders::normal_light_frag_spirv);
    info.entrypoint = shaders::normal_light_entry;
  }
  else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = shaders::normal_light_frag_msl;
    info.code_size = sizeof(shaders::normal_light_frag_msl);
    info.entrypoint = shaders::normal_light_entry_msl;
  }
  else {
    device_ = nullptr;
    return false;
  }
  shader_ = SDL_CreateGPUShader(device_, &info);
  if (!shader_) {
    Logger::warn("Normal-map lighting off: the shader did not load ({})",
                 SDL_GetError());
    device_ = nullptr;
    return false;
  }
  SDL_GPUSamplerCreateInfo sampler{};
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler_ = SDL_CreateGPUSampler(device_, &sampler);
  if (!sampler_) {
    SDL_ReleaseGPUShader(device_, shader_);
    shader_ = nullptr;
    device_ = nullptr;
    return false;
  }
  failed_ = false;
  return true;
}

bool NormalLightPass::draw(SDL_Texture* picture, SDL_Texture* normals,
                           const SDL_FRect& src, const SDL_FRect& dst,
                           const NormalLights& lights) {
  if (!ready() || !picture || !normals) {
    return false;
  }
  auto* gpu_normals = static_cast<SDL_GPUTexture*>(
      SDL_GetPointerProperty(SDL_GetTextureProperties(normals),
                             SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
  if (!gpu_normals) {
    return false;
  }
  // Pictures come and go; a screen uses a few dozen.
  constexpr std::size_t max_states = 256;
  if (states_.size() >= max_states && !states_.contains(gpu_normals)) {
    reset();
  }
  SDL_GPURenderState*& state = states_[gpu_normals];
  if (!state) {
    const SDL_GPUTextureSamplerBinding binding{gpu_normals, sampler_};
    SDL_GPURenderStateCreateInfo info{};
    info.fragment_shader = shader_;
    info.num_sampler_bindings = 1;
    info.sampler_bindings = &binding;
    state = SDL_CreateGPURenderState(renderer_, &info);
    if (!state) {
      states_.erase(gpu_normals);
      return false;
    }
  }
  const NormalLightUniforms u = make_uniforms(lights);
  SDL_SetGPURenderStateFragmentUniforms(state, 0, &u, sizeof(u));
  SDL_SetGPURenderState(renderer_, state);
  SDL_RenderTexture(renderer_, picture, &src, &dst);
  SDL_SetGPURenderState(renderer_, nullptr);
  return true;
}

} // namespace fx
