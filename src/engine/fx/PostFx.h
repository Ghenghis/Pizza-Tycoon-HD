/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <array>
#include "engine/fx/Grade.h"
#include "engine/fx/Look.h"

namespace fx {

// The uniforms of shaders/postfx.frag.hlsl, eight float4s (128 bytes), in
// the order the shader declares them.
struct PostFxUniforms {
  std::array<float, 4> tone{};    // exposure, contrast, saturation, -
  std::array<float, 4> balance{}; // white balance r, g, b, -
  std::array<float, 4> lift{};
  std::array<float, 4> gamma{};
  std::array<float, 4> gain{};
  std::array<float, 4> bloom{};  // strength, threshold, texel w, texel h
  std::array<float, 4> frame{};  // vignette, crt, scan rows, mask columns
  std::array<float, 4> output{}; // split u, linear io, SDR white, headroom
};
static_assert(sizeof(PostFxUniforms) == 128);

struct OutputInfo {
  bool linear = false; // scRGB output (SDL_COLORSPACE_SRGB_LINEAR)
  float sdr_white = 1.0f;
  float headroom = 1.0f;
  float bloom_w = 1.0f; // bloom source size, texels
  float bloom_h = 1.0f;
};
[[nodiscard]] PostFxUniforms
make_uniforms(const Grade& g, const FrameInfo& frame, const OutputInfo& out);

// The final picture pass: draws the composited frame to the window with
// the grade (see Look.h).
//
//   * Identity grade: one plain SDL_RenderTexture, exactly as before.
//   * SDL's "gpu" render driver: the frame goes through the post-FX shader
//     (an SDL_GPURenderState), bloom, vignette and CRT included.
//   * Any other driver: what fixed-function SDL can do, exposure and white
//     balance as a colour modulation; the rest of the grade is left out
//     (the options screen says it needs the GPU renderer).
class PostFx {
public:
  explicit PostFx(SDL_Renderer* renderer);
  ~PostFx();
  PostFx(const PostFx&) = delete;
  PostFx& operator=(const PostFx&) = delete;

  enum class Path : uint8_t { Plain, Shader, Modulation };

  // Draws `frame` over the whole current target (the window, through its
  // logical presentation), as SDL_RenderTexture(r, frame, NULL, NULL) does.
  // Returns how it was drawn.
  Path draw(SDL_Texture* frame, const Grade& grade, const FrameInfo& info);

  // True when the renderer is SDL's GPU driver and the shader loaded (it is
  // loaded on first use; false after a failure, which is logged once).
  [[nodiscard]] bool shader_ready();
  // SDL_EVENT_RENDER_TARGETS_RESET / SDL_EVENT_RENDER_DEVICE_RESET.
  void on_render_reset(bool device_lost);

  // True for SDL's GPU render driver ("gpu").
  [[nodiscard]] static bool is_gpu_driver(SDL_Renderer* renderer);

private:
  bool ensure_shader();
  bool ensure_chain(int w, int h);
  bool ensure_state();
  void fill_bloom(SDL_Texture* frame);
  void draw_modulated(SDL_Texture* frame, const Grade& grade,
                      const FrameInfo& info);
  void release();

  SDL_Renderer* renderer_;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUShader* shader_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  SDL_GPURenderState* state_ = nullptr;
  // The bloom source: 1/2, 1/4 and 1/8 of the frame.
  std::array<SDL_Texture*, 3> chain_{};
  SDL_GPUTexture* state_bloom_ = nullptr; // what state_ samples
  bool failed_ = false;
};

} // namespace fx
