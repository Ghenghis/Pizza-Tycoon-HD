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

namespace fx {
class NormalLightPass;
struct NormalLights;
} // namespace fx

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
  // art_from_x (texels of the layer): HD art is drawn only from there to
  // the right, the original pixels to the left of it (the 1994 look, and
  // the two side by side); HD text is drawn everywhere. 0 is all HD art,
  // the layer's width or more none.
  void draw_layer(SDL_Texture* target, const gfx::Surface& layer, float fade,
                  int art_from_x = 0);

  // The glow of the layer's emissive art (fx::MapStore emissive maps), for
  // night: `target` (the layer's size times hd_scale()) is cleared to
  // transparent black, each picture with an emissive map adds its light
  // (premultiplied), and each picture drawn after it covers that light as
  // far as it is opaque, so the glow is only where the art shows. Left of
  // art_from_x there is no glow (the 1994 look). Returns false, leaving
  // `target` untouched, when no picture of the layer has an emissive map.
  bool draw_emissive(SDL_Texture* target, const gfx::Surface& layer,
                     int art_from_x = 0);
  // True when some picture of the layer has an emissive map.
  [[nodiscard]] bool has_emissive(const gfx::Surface& layer) const;

  // Normal-map lighting for the next draw_layer calls (SDL's GPU driver):
  // pictures with a normal map (fx::MapStore) are drawn through `pass`
  // with `lights`. Null (the default) draws every picture as before.
  void normal_lighting(fx::NormalLightPass* pass,
                       const fx::NormalLights* lights) {
    normal_pass_ = pass;
    normal_lights_ = lights;
  }
  // Pictures drawn with their normal map in the last draw_layer.
  [[nodiscard]] std::size_t normal_lit() const noexcept { return normal_lit_; }

  // Where the GPU copy of a picture is: its texture and the top-left of the
  // picture in it (not 0,0 for pictures in a shared atlas page).
  struct Placed {
    SDL_Texture* tex = nullptr;
    int x = 0;
    int y = 0;
  };
  // Uploaded on first use.
  Placed place(const std::shared_ptr<const gfx::HdImage>& img);
  SDL_Texture* texture_for(const std::shared_ptr<const gfx::HdImage>& img) {
    return place(img).tex;
  }

  // Streams pictures to the GPU ahead of use, a band of rows at a time,
  // spending at most budget_ms per call (call once per frame). Pictures not
  // finished when they are first drawn are completed then.
  void stream(const std::vector<std::shared_ptr<const gfx::HdImage>>& add,
              double budget_ms);
  [[nodiscard]] std::size_t streaming() const noexcept {
    return stream_queue_.size();
  }

  // Uploads pictures ahead of use (a screen's art while it loads), up to
  // `budget_bytes` this call. Returns how many bytes were uploaded.
  std::size_t
  preload(const std::vector<std::shared_ptr<const gfx::HdImage>>& imgs,
          std::size_t budget_bytes);

  // Creates the first atlas page for small pictures ahead of use.
  void prepare_atlas();

  // Call once per frame: forgets pictures nobody holds any more and evicts
  // the least recently used ones when over the memory budget.
  void end_frame();

  [[nodiscard]] std::size_t texture_bytes() const noexcept { return bytes_; }
  // GPU textures held: pictures of their own plus atlas pages.
  [[nodiscard]] std::size_t texture_count() const noexcept;
  // Pictures on the GPU, in textures of their own or in atlas pages.
  [[nodiscard]] std::size_t picture_count() const noexcept {
    return cache_.size();
  }
  [[nodiscard]] std::size_t atlas_pages() const noexcept;
  void memory_budget(std::size_t bytes) noexcept { budget_ = bytes; }

  // Stats of the last draw_layer, for tests and the frame-time overlay.
  struct Stats {
    std::size_t quads = 0;     // HD pictures drawn
    std::size_t batches = 0;   // draw calls they took (1:1 ones are batched)
    std::size_t base_runs = 0; // indexed pixel runs redrawn between them
  };
  [[nodiscard]] const Stats& last_stats() const noexcept { return stats_; }
  // Time spent preparing and uploading textures, since construction.
  [[nodiscard]] double upload_ms() const noexcept { return upload_ms_; }
  // Small pictures put in atlas pages, their bytes, and the bytes sent to
  // the GPU for them, since construction.
  struct AtlasStats {
    std::size_t pictures = 0;
    std::size_t bytes = 0;
    std::size_t sent = 0;
  };
  [[nodiscard]] AtlasStats atlas_stats() const noexcept {
    return {atlas_pictures_, atlas_bytes_, sent_bytes_};
  }

private:
  struct Entry {
    std::weak_ptr<const gfx::HdImage> img;
    SDL_Texture* tex = nullptr;
    std::size_t bytes = 0; // of its own texture; 0 in an atlas page
    uint64_t last_used = 0;
    int rows_done = 0; // rows uploaded so far; complete at img height
    int page = -1;     // atlas page, or -1 for a texture of its own
    int x = 0;         // top-left of the picture in the texture
    int y = 0;
  };
  // A shared texture for small pictures (gfx::HdImage::atlas), filled in
  // shelves: rows of slots of similar height. Creating a GPU texture costs
  // far more than uploading a small picture, and a screen full of text has
  // dozens of them; one page also lets SDL batch the text into one draw.
  // A page is reused from the top once every picture in it is gone.
  struct Shelf {
    int y = 0;
    int h = 0;
    int x = 0; // next free column
  };
  struct Page {
    SDL_Texture* tex = nullptr;
    std::vector<Shelf> shelves;
    int bottom = 0; // first row below the last shelf
    int live = 0;   // pictures in it
    // The page's texels (premultiplied) on the CPU: new pictures are written
    // here and sent in one upload of the changed rows (each upload is a
    // staging texture and a copy on Direct3D, whatever its size).
    std::vector<uint32_t> texels;
    int dirty_x0 = 0;
    int dirty_x1 = 0;
    int dirty_y0 = 0;
    int dirty_y1 = 0; // empty when equal to dirty_y0
  };
  struct Run {
    uint32_t floor;
    int y, x0, x1;
  };

  void upload_base(const gfx::Surface& layer, float fade);
  // Draws the quads gathered for batch_tex_ in one call.
  void flush_batch();
  void draw_runs(const std::vector<Run>& runs, std::size_t& pos, uint32_t floor,
                 int k);
  void evict(Entry& e);
  // Creates the texture (nothing uploaded yet), or null when impossible.
  Entry* create(const std::shared_ptr<const gfx::HdImage>& img);
  void size_atlas();
  // A new atlas page; its index, or -1.
  int add_page();
  // place() without sending the atlas pages (see flush_atlas).
  Placed locate(const std::shared_ptr<const gfx::HdImage>& img);
  // Uploads what changed in the atlas pages.
  void flush_atlas();
  // On the GPU and complete: drawable.
  [[nodiscard]] bool ready(const Entry& e, const gfx::HdImage& img) const;
  // A slot for the picture in an atlas page, or null when it doesn't fit
  // one (it then gets a texture of its own).
  Entry* create_in_atlas(const std::shared_ptr<const gfx::HdImage>& img);
  // Premultiplies and uploads up to `rows` more rows.
  void upload_rows(Entry& e, const gfx::HdImage& img, int rows);

  SDL_Renderer* renderer_;
  std::unordered_map<const gfx::HdImage*, Entry> cache_;
  SDL_Texture* base_ = nullptr;
  int base_w_ = 0;
  int base_h_ = 0;
  std::vector<uint32_t> scratch_;
  std::vector<Run> runs_;
  bool batch_geometry_ = true;
  SDL_Texture* batch_tex_ = nullptr;
  std::vector<SDL_Vertex> batch_vertices_;
  std::vector<int> batch_indices_;
  uint64_t frame_ = 1;
  std::size_t bytes_ = 0;
  std::size_t budget_ = std::size_t{1536} << 20; // 1.5 GiB of textures
  int max_texture_size_ = 16384;
  std::vector<std::weak_ptr<const gfx::HdImage>> stream_queue_;
  std::vector<Page> pages_;
  int page_w_ = 0; // atlas page size, set on first use
  int page_h_ = 0;
  double upload_ms_ = 0;
  fx::NormalLightPass* normal_pass_ = nullptr;
  const fx::NormalLights* normal_lights_ = nullptr;
  std::size_t normal_lit_ = 0;
  std::size_t atlas_pictures_ = 0;
  std::size_t atlas_bytes_ = 0;
  std::size_t sent_bytes_ = 0;
  Stats stats_;
};
