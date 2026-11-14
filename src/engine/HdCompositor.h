/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Surface.h"

// True unless the environment variable PLEGACY_CPU_COMPOSE=1 asks for the
// old CPU compositing (a fallback for broken GPU drivers, and for checks).
[[nodiscard]] bool hd_gpu_compositing();

// Draws HD surfaces on the GPU. A layer's indexed pixels are expanded at
// engine resolution (640x400, cheap) and its HD pictures are drawn from GPU
// textures as quads, following the surface's draw list (gfx::HdList), so the
// CPU never touches the hd_scale()^2 texels per pixel.
//
// Works with every SDL renderer (Direct3D 11/12, Vulkan, Metal, OpenGL and
// the software renderer used by the tests): only textured rect copies and
// premultiplied-alpha blending are used, no custom shaders.
class HdCompositor {
public:
  explicit HdCompositor(SDL_Renderer* renderer);
  ~HdCompositor();
  HdCompositor(const HdCompositor&) = delete;
  HdCompositor& operator=(const HdCompositor&) = delete;

  // Replaces the contents of `target` (a render target of the layer's size
  // times hd_scale(), premultiplied alpha) with the layer. fade 1 = normal,
  // 0 = black. Leaves the render target set to `target`.
  void draw_layer(SDL_Texture* target, const gfx::Surface& layer, float fade);

  // The GPU copy of a picture, uploaded on first use.
  SDL_Texture* texture_for(const std::shared_ptr<const gfx::HdImage>& img);

  // Uploads pictures ahead of use (a screen's art while it loads), up to
  // `budget_bytes` this call. Returns how many bytes were uploaded.
  std::size_t
  preload(const std::vector<std::shared_ptr<const gfx::HdImage>>& imgs,
          std::size_t budget_bytes);

  // Call once per frame: forgets pictures nobody holds any more and evicts
  // the least recently used ones when over the memory budget.
  void end_frame();

  [[nodiscard]] std::size_t texture_bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t texture_count() const noexcept {
    return cache_.size();
  }
  void memory_budget(std::size_t bytes) noexcept { budget_ = bytes; }

  // Stats of the last draw_layer, for tests and the frame-time overlay.
  struct Stats {
    std::size_t quads = 0;     // HD pictures drawn
    std::size_t base_runs = 0; // indexed pixel runs redrawn between them
  };
  [[nodiscard]] const Stats& last_stats() const noexcept { return stats_; }
  // Time spent preparing and uploading textures, since construction.
  [[nodiscard]] double upload_ms() const noexcept { return upload_ms_; }

private:
  struct Entry {
    std::weak_ptr<const gfx::HdImage> img;
    SDL_Texture* tex = nullptr;
    std::size_t bytes = 0;
    uint64_t last_used = 0;
  };
  struct Run {
    uint32_t floor;
    int y, x0, x1;
  };

  void upload_base(const gfx::Surface& layer, float fade);
  void draw_runs(const std::vector<Run>& runs, std::size_t& pos, uint32_t floor,
                 int k);
  void evict(Entry& e);

  SDL_Renderer* renderer_;
  std::unordered_map<const gfx::HdImage*, Entry> cache_;
  SDL_Texture* base_ = nullptr;
  int base_w_ = 0;
  int base_h_ = 0;
  std::vector<uint32_t> scratch_;
  std::vector<Run> runs_;
  uint64_t frame_ = 1;
  std::size_t bytes_ = 0;
  std::size_t budget_ = std::size_t{1536} << 20; // 1.5 GiB of textures
  int max_texture_size_ = 16384;
  double upload_ms_ = 0;
  Stats stats_;
};
