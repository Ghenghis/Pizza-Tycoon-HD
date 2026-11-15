/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/PostFx.h"

#include <algorithm>
#include <cstring>
#include "engine/Logger.h"
#include "engine/fx/generated/PostFxShaders.h"

namespace fx {

PostFxUniforms make_uniforms(const Grade& g, const FrameInfo& frame,
                             const OutputInfo& out) {
  PostFxUniforms u;
  const std::array<float, 3> bal = g.balance();
  u.tone = {g.exposure, g.contrast, g.saturation, 0.0f};
  u.balance = {bal[0], bal[1], bal[2], 0.0f};
  u.lift = {g.lift[0], g.lift[1], g.lift[2], 0.0f};
  u.gamma = {g.gamma[0], g.gamma[1], g.gamma[2], 1.0f};
  u.gain = {g.gain[0], g.gain[1], g.gain[2], 1.0f};
  u.bloom = {g.bloom, g.bloom_threshold, 1.0f / std::max(out.bloom_w, 1.0f),
             1.0f / std::max(out.bloom_h, 1.0f)};
  u.frame = {g.vignette, g.crt, frame.scan_rows, frame.mask_cols};
  u.output = {frame.split_u, out.linear ? 1.0f : 0.0f, out.sdr_white,
              out.headroom};
  return u;
}

PostFx::PostFx(SDL_Renderer* renderer) : renderer_(renderer) {}

PostFx::~PostFx() { release(); }

bool PostFx::is_gpu_driver(SDL_Renderer* renderer) {
  const char* name = renderer ? SDL_GetRendererName(renderer) : nullptr;
  return name && std::strcmp(name, SDL_GPU_RENDERER) == 0;
}

void PostFx::release() {
  if (state_) {
    SDL_DestroyGPURenderState(state_);
    state_ = nullptr;
  }
  state_bloom_ = nullptr;
  for (SDL_Texture*& tex : chain_) {
    if (tex) {
      SDL_DestroyTexture(tex);
      tex = nullptr;
    }
  }
  if (device_) {
    if (sampler_) {
      SDL_ReleaseGPUSampler(device_, sampler_);
    }
    if (shader_) {
      SDL_ReleaseGPUShader(device_, shader_);
    }
  }
  sampler_ = nullptr;
  shader_ = nullptr;
  device_ = nullptr;
}

void PostFx::on_render_reset(bool device_lost) {
  // The bloom copies are made again every frame that needs them; only a new
  // device needs the shader made again.
  if (device_lost) {
    release();
    failed_ = false;
  }
}

bool PostFx::shader_ready() { return ensure_shader(); }

bool PostFx::ensure_shader() {
  if (shader_) {
    return true;
  }
  if (failed_) {
    return false;
  }
  failed_ = true; // until it works
  if (!is_gpu_driver(renderer_)) {
    return false;
  }
  device_ = SDL_GetGPURendererDevice(renderer_);
  if (!device_) {
    Logger::warn("Post-FX off: the GPU renderer has no device ({})",
                 SDL_GetError());
    return false;
  }
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
  SDL_GPUShaderCreateInfo info{};
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 2;
  info.num_uniform_buffers = 1;
  const char* format_name = nullptr;
  if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
    info.code = shaders::postfx_frag_dxil;
    info.code_size = sizeof(shaders::postfx_frag_dxil);
    info.entrypoint = shaders::postfx_entry;
    format_name = "DXIL";
  }
  else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = shaders::postfx_frag_spirv;
    info.code_size = sizeof(shaders::postfx_frag_spirv);
    info.entrypoint = shaders::postfx_entry;
    format_name = "SPIR-V";
  }
  else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.code = shaders::postfx_frag_msl;
    info.code_size = sizeof(shaders::postfx_frag_msl);
    info.entrypoint = shaders::postfx_entry_msl;
    format_name = "MSL";
  }
  else {
    Logger::warn("Post-FX off: the GPU device takes no shader format the "
                 "game ships (DXIL, SPIR-V or MSL)");
    device_ = nullptr;
    return false;
  }
  shader_ = SDL_CreateGPUShader(device_, &info);
  if (!shader_) {
    Logger::warn("Post-FX off: the {} shader did not load ({})", format_name,
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
    Logger::warn("Post-FX off: no sampler ({})", SDL_GetError());
    release();
    return false;
  }
  Logger::info("Post-FX shader loaded ({})", format_name);
  failed_ = false;
  return true;
}

bool PostFx::ensure_chain(int w, int h) {
  int cw = w;
  int ch = h;
  for (SDL_Texture*& tex : chain_) {
    cw = std::max(1, cw / 2);
    ch = std::max(1, ch / 2);
    if (tex && tex->w == cw && tex->h == ch) {
      continue;
    }
    if (tex) {
      SDL_DestroyTexture(tex);
    }
    tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_TARGET, cw, ch);
    if (!tex) {
      Logger::warn("Post-FX bloom copy not made ({})", SDL_GetError());
      return false;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
  }
  return true;
}

bool PostFx::ensure_state() {
  auto* bloom = static_cast<SDL_GPUTexture*>(
      SDL_GetPointerProperty(SDL_GetTextureProperties(chain_.back()),
                             SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
  if (!bloom) {
    return false;
  }
  if (state_ && state_bloom_ == bloom) {
    return true;
  }
  if (state_) {
    SDL_DestroyGPURenderState(state_);
    state_ = nullptr;
  }
  const SDL_GPUTextureSamplerBinding binding{bloom, sampler_};
  SDL_GPURenderStateCreateInfo info{};
  info.fragment_shader = shader_;
  info.num_sampler_bindings = 1;
  info.sampler_bindings = &binding;
  state_ = SDL_CreateGPURenderState(renderer_, &info);
  if (!state_) {
    Logger::warn("Post-FX off: no render state ({})", SDL_GetError());
    release();
    failed_ = true;
    return false;
  }
  state_bloom_ = bloom;
  return true;
}

void PostFx::fill_bloom(SDL_Texture* frame) {
  SDL_Texture* target = SDL_GetRenderTarget(renderer_);
  SDL_ScaleMode scale = SDL_SCALEMODE_NEAREST;
  SDL_BlendMode blend = SDL_BLENDMODE_NONE;
  SDL_GetTextureScaleMode(frame, &scale);
  SDL_GetTextureBlendMode(frame, &blend);
  SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_NONE);
  // Each halving samples between four texels: a 2x2 box.
  SDL_Texture* source = frame;
  for (SDL_Texture* tex : chain_) {
    SDL_SetRenderTarget(renderer_, tex);
    SDL_RenderTexture(renderer_, source, nullptr, nullptr);
    source = tex;
  }
  SDL_SetTextureScaleMode(frame, scale);
  SDL_SetTextureBlendMode(frame, blend);
  SDL_SetRenderTarget(renderer_, target);
}

namespace {

// The size the frame is drawn at: the logical size when drawing to the
// window with a logical presentation, else the target's size (a texture
// target, as in a graded screenshot, ignores the presentation).
SDL_FPoint logical_size(SDL_Renderer* r) {
  int w = 0;
  int h = 0;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  SDL_GetRenderLogicalPresentation(r, &w, &h, &mode);
  if (w <= 0 || h <= 0 || mode == SDL_LOGICAL_PRESENTATION_DISABLED ||
      SDL_GetRenderTarget(r) != nullptr) {
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
  }
  return {static_cast<float>(w), static_cast<float>(h)};
}

} // namespace

void PostFx::draw_modulated(SDL_Texture* frame, const Grade& grade,
                            const FrameInfo& info) {
  const std::array<float, 3> m = grade.modulation();
  // Colour modulation only darkens; anything above 1 comes from the colour
  // scale (the software renderer clamps that at 1).
  const float scale = std::max({m[0], m[1], m[2], 1.0f});
  float old_scale = 1.0f;
  float mr = 1.0f;
  float mg = 1.0f;
  float mb = 1.0f;
  SDL_GetRenderColorScale(renderer_, &old_scale);
  SDL_GetTextureColorModFloat(frame, &mr, &mg, &mb);

  const SDL_FPoint size = logical_size(renderer_);
  const float split = std::clamp(info.split_u, 0.0f, 1.0f);
  SDL_FRect src{0, 0, static_cast<float>(frame->w),
                static_cast<float>(frame->h)};
  SDL_FRect dst{0, 0, size.x, size.y};
  if (split > 0.0f) {
    const SDL_FRect left_src{0, 0, src.w * split, src.h};
    const SDL_FRect left_dst{0, 0, dst.w * split, dst.h};
    SDL_RenderTexture(renderer_, frame, &left_src, &left_dst);
    src = {left_src.w, 0, src.w - left_src.w, src.h};
    dst = {left_dst.w, 0, dst.w - left_dst.w, dst.h};
  }
  SDL_SetTextureColorModFloat(frame, mr * m[0] / scale, mg * m[1] / scale,
                              mb * m[2] / scale);
  if (scale > 1.0f) {
    SDL_SetRenderColorScale(renderer_, old_scale * scale);
  }
  SDL_RenderTexture(renderer_, frame, &src, &dst);
  SDL_SetRenderColorScale(renderer_, old_scale);
  SDL_SetTextureColorModFloat(frame, mr, mg, mb);
}

PostFx::Path PostFx::draw(SDL_Texture* frame, const Grade& grade,
                          const FrameInfo& info) {
  if (grade.picture_identity() || info.split_u >= 1.0f) {
    SDL_RenderTexture(renderer_, frame, nullptr, nullptr);
    return Path::Plain;
  }
  if (!ensure_shader() || !ensure_chain(frame->w, frame->h) ||
      !ensure_state()) {
    draw_modulated(frame, grade, info);
    return Path::Modulation;
  }
  if (grade.bloom > 0.0f) {
    fill_bloom(frame);
  }
  OutputInfo out;
  const SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
  out.linear =
      SDL_GetNumberProperty(props, SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER,
                            SDL_COLORSPACE_SRGB) == SDL_COLORSPACE_SRGB_LINEAR;
  if (out.linear) {
    out.sdr_white = SDL_GetFloatProperty(
        props, SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT, 1.0f);
    out.headroom =
        SDL_GetFloatProperty(props, SDL_PROP_RENDERER_HDR_HEADROOM_FLOAT, 1.0f);
  }
  out.bloom_w = static_cast<float>(chain_.back()->w);
  out.bloom_h = static_cast<float>(chain_.back()->h);
  FrameInfo frame_info = info;
  // The aperture mask follows the screen's own pixels.
  SDL_FRect shown{};
  if (!SDL_GetRenderTarget(renderer_) &&
      SDL_GetRenderLogicalPresentationRect(renderer_, &shown) && shown.w > 0) {
    frame_info.mask_cols = shown.w;
  }
  const PostFxUniforms u = make_uniforms(grade, frame_info, out);
  SDL_SetGPURenderStateFragmentUniforms(state_, 0, &u, sizeof(u));
  SDL_SetGPURenderState(renderer_, state_);
  SDL_RenderTexture(renderer_, frame, nullptr, nullptr);
  SDL_SetGPURenderState(renderer_, nullptr);
  return Path::Shader;
}

} // namespace fx
