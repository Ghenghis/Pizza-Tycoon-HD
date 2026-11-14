/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/HdCompositor.h"

#include <algorithm>
#include <thread>
#include "engine/Fatal.h"
#include "engine/Logger.h"
#include "engine/gfx/HdList.h"

using gfx::HdImage;
using gfx::HdList;

namespace {

uint32_t premultiply(uint32_t p) {
  const uint32_t a = gfx::alpha(p);
  if (a == 255) {
    return p;
  }
  if (a == 0) {
    return 0;
  }
  auto m = [a](uint32_t c) { return (c * a + 127) / 255; };
  return gfx::pack_rgba(m(gfx::red(p)), m(gfx::green(p)), m(gfx::blue(p)), a);
}

} // namespace

bool hd_gpu_compositing() {
  static const bool on = [] {
    const char* v = SDL_getenv("PLEGACY_CPU_COMPOSE");
    return !(v && v[0] == '1');
  }();
  return on;
}

HdCompositor::HdCompositor(SDL_Renderer* renderer) : renderer_(renderer) {
  const SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
  const auto max = SDL_GetNumberProperty(
      props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
  if (max > 0) {
    max_texture_size_ = static_cast<int>(max);
  }
}

HdCompositor::~HdCompositor() {
  for (auto& [ptr, e] : cache_) {
    SDL_DestroyTexture(e.tex);
  }
  if (base_) {
    SDL_DestroyTexture(base_);
  }
}

SDL_Texture*
HdCompositor::texture_for(const std::shared_ptr<const HdImage>& img) {
  auto it = cache_.find(img.get());
  if (it != cache_.end()) {
    if (it->second.img.lock() == img) {
      it->second.last_used = frame_;
      return it->second.tex;
    }
    // Same address, new picture: the old one is gone.
    evict(it->second);
    cache_.erase(it);
  }
  if (img->width > max_texture_size_ || img->height > max_texture_size_ ||
      img->width <= 0 || img->height <= 0) {
    Logger::warn("HD picture {}x{} is larger than the GPU allows ({})",
                 img->width, img->height, max_texture_size_);
    return nullptr;
  }
  const uint64_t started = SDL_GetPerformanceCounter();
  SDL_Texture* tex =
      SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_STATIC, img->width, img->height);
  if (!tex) {
    Logger::warn("Cannot create HD texture: {}", SDL_GetError());
    return nullptr;
  }
  scratch_.resize(img->pixels.size());
  // Big pictures (a 4K screen is 9 million texels) are split across cores.
  const std::size_t n = img->pixels.size();
  const unsigned workers =
      n < 1'000'000
          ? 1u
          : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  {
    std::vector<std::jthread> pool;
    const std::size_t step = (n + workers - 1) / workers;
    for (unsigned w = 1; w < workers; ++w) {
      const std::size_t a = std::min(n, w * step);
      const std::size_t b = std::min(n, a + step);
      pool.emplace_back([&, a, b] {
        std::transform(img->pixels.begin() + static_cast<std::ptrdiff_t>(a),
                       img->pixels.begin() + static_cast<std::ptrdiff_t>(b),
                       scratch_.begin() + static_cast<std::ptrdiff_t>(a),
                       premultiply);
      });
    }
    std::transform(img->pixels.begin(),
                   img->pixels.begin() +
                       static_cast<std::ptrdiff_t>(std::min(n, step)),
                   scratch_.begin(), premultiply);
  }
  SDL_UpdateTexture(tex, nullptr, scratch_.data(), img->width * 4);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  upload_ms_ += static_cast<double>(SDL_GetPerformanceCounter() - started) *
                1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
  Entry e;
  e.img = img;
  e.tex = tex;
  e.bytes = img->pixels.size() * 4;
  e.last_used = frame_;
  bytes_ += e.bytes;
  cache_.emplace(img.get(), std::move(e));
  return tex;
}

std::size_t
HdCompositor::preload(const std::vector<std::shared_ptr<const HdImage>>& imgs,
                      std::size_t budget_bytes) {
  std::size_t done = 0;
  for (const auto& img : imgs) {
    if (done >= budget_bytes) {
      break;
    }
    auto it = cache_.find(img.get());
    if (it != cache_.end() && it->second.img.lock() == img) {
      continue;
    }
    if (texture_for(img)) {
      done += img->pixels.size() * 4;
    }
  }
  return done;
}

void HdCompositor::evict(Entry& e) {
  SDL_DestroyTexture(e.tex);
  bytes_ -= e.bytes;
  e.tex = nullptr;
}

void HdCompositor::end_frame() {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.img.expired()) {
      evict(it->second);
      it = cache_.erase(it);
    }
    else {
      ++it;
    }
  }
  if (bytes_ > budget_) {
    std::vector<std::pair<uint64_t, const HdImage*>> lru;
    for (const auto& [ptr, e] : cache_) {
      if (e.last_used < frame_) {
        lru.emplace_back(e.last_used, ptr);
      }
    }
    std::sort(lru.begin(), lru.end());
    for (const auto& [used, ptr] : lru) {
      if (bytes_ <= budget_) {
        break;
      }
      auto it = cache_.find(ptr);
      evict(it->second);
      cache_.erase(it);
    }
  }
  ++frame_;
}

void HdCompositor::upload_base(const gfx::Surface& layer, float fade) {
  const int w = layer.width();
  const int h = layer.height();
  if (!base_ || base_w_ != w || base_h_ != h) {
    if (base_) {
      SDL_DestroyTexture(base_);
    }
    base_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!base_) {
      fatal(SDL_GetError());
    }
    SDL_SetTextureScaleMode(base_, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(base_, SDL_BLENDMODE_NONE);
    base_w_ = w;
    base_h_ = h;
  }
  // Index 0 is the transparent colour key; everything else is opaque, so
  // premultiplied and straight alpha are the same here.
  uint32_t lut[256];
  std::fill(std::begin(lut), std::end(lut), gfx::pack_rgba(0, 0, 0, 255));
  const auto& pal = layer.colors();
  for (std::size_t i = 0; i < pal.size() && i < 256; ++i) {
    lut[i] = gfx::pack_rgba(
        static_cast<uint32_t>(static_cast<float>(pal[i].r) * fade),
        static_cast<uint32_t>(static_cast<float>(pal[i].g) * fade),
        static_cast<uint32_t>(static_cast<float>(pal[i].b) * fade), 255);
  }
  lut[0] = 0;
  const auto px = layer.pixels();
  scratch_.resize(px.size());
  for (std::size_t i = 0; i < px.size(); ++i) {
    scratch_[i] = lut[px[i]];
  }
  SDL_UpdateTexture(base_, nullptr, scratch_.data(), w * 4);
}

void HdCompositor::draw_runs(const std::vector<Run>& runs, std::size_t& pos,
                             uint32_t floor, int k) {
  // Rect copies, not a triangle list: exact on every backend (texture
  // coordinates at texel edges round differently between rasterizers), and
  // SDL batches consecutive copies of one texture into one draw anyway.
  const auto fk = static_cast<float>(k);
  for (; pos < runs.size() && runs[pos].floor == floor; ++pos) {
    const Run& r = runs[pos];
    const SDL_FRect src{static_cast<float>(r.x0), static_cast<float>(r.y),
                        static_cast<float>(r.x1 - r.x0), 1.0f};
    const SDL_FRect dst{src.x * fk, src.y * fk, src.w * fk, fk};
    SDL_RenderTexture(renderer_, base_, &src, &dst);
    ++stats_.base_runs;
  }
}

void HdCompositor::draw_layer(SDL_Texture* target, const gfx::Surface& layer,
                              float fade) {
  stats_ = {};
  const int k = gfx::hd_scale();
  const int w = layer.width();
  const int h = layer.height();
  upload_base(layer, fade);
  SDL_SetRenderTarget(renderer_, target);

  // 1. The indexed pixels, everywhere (replacing whatever was there).
  const SDL_FRect src_all{0, 0, static_cast<float>(w), static_cast<float>(h)};
  const SDL_FRect dst_all{0, 0, static_cast<float>(w * k),
                          static_cast<float>(h * k)};
  SDL_RenderTexture(renderer_, base_, &src_all, &dst_all);

  const HdList* list = layer.hd_list();
  if (!list) {
    return;
  }

  // 2. Runs of indexed pixels that must be redrawn just before command f,
  //    because a plain write or a soft-edged replacement hid the commands
  //    before f there (see HdList.h).
  runs_.clear();
  if (!list->is_leaf()) {
    for (int y = 0; y < h; ++y) {
      int x = 0;
      while (x < w) {
        const uint32_t f = list->floor_of(x, y);
        if (f == 0 || !list->needs_base(x, y)) {
          ++x;
          continue;
        }
        const int x0 = x;
        while (x < w && list->floor_of(x, y) == f && list->needs_base(x, y)) {
          ++x;
        }
        runs_.push_back({f, y, x0, x});
      }
    }
    std::stable_sort(
        runs_.begin(), runs_.end(),
        [](const Run& a, const Run& b) { return a.floor < b.floor; });
  }

  // 3. The pictures, in order, with those runs in between.
  std::size_t pos = 0;
  const auto& cmds = list->cmds();
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    draw_runs(runs_, pos, static_cast<uint32_t>(i), k);
    const gfx::HdCmd& c = cmds[i];
    SDL_Texture* tex = texture_for(c.img);
    if (!tex) {
      continue;
    }
    SDL_SetTextureColorModFloat(tex, fade, fade, fade);
    SDL_SetTextureScaleMode(tex, c.half ? SDL_SCALEMODE_LINEAR
                                        : SDL_SCALEMODE_NEAREST);
    const SDL_FRect src{static_cast<float>(c.sx), static_cast<float>(c.sy),
                        static_cast<float>(c.sw), static_cast<float>(c.sh)};
    const SDL_FRect dst{static_cast<float>(c.dx), static_cast<float>(c.dy),
                        static_cast<float>(c.dw()), static_cast<float>(c.dh())};
    SDL_RenderTexture(renderer_, tex, &src, &dst);
    ++stats_.quads;
  }
  draw_runs(runs_, pos, static_cast<uint32_t>(cmds.size()), k);
}
