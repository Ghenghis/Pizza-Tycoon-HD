/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/Renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <thread>
#include <vector>
#include "engine/DisplayConstants.h"
#include "engine/Fatal.h"
#include "engine/Renderable.h"
#include "engine/SDL_include.h"
#include "engine/adapters/SDLRectAdapter.h"
#include "engine/gfx/Surface.h"
#include "engine/ui/CursorManager.h"

using gfx::Surface;

void Renderer::add_layers(Renderable* r) {
  auto layers = r->get_layers();
  for (const auto& layer : layers) {
    layers_.push_back(layer);
  }
}

void Renderer::render() {
  // For now all layers have to be full screen size
  ensure_backbuffer(frame_width(), frame_height());

  // During fade, clear cache so all layers regenerate with current fade_factor
  if (is_fading()) {
    cache_.clear();
    force_recompose_ = true;
  }

  // propagate force-dirty from layers
  for (auto& layer : layers_) {
    force_recompose_ |= layer.view->needs_force_recompose();
  }

  bool below_changed = force_recompose_;
  bool frame_changed = force_recompose_;

  for (auto& layer : layers_) {
    Renderable* view = layer.view;
    ViewRect r = layer.area;
    auto width = r.w;
    auto height = r.h;

    auto& layer_surface = layer_surfaces_[view];
    if (!layer_surface || layer_surface->width() != width ||
        layer_surface->height() != height) {
      layer_surface = std::make_unique<Surface>(width, height);
      view->mark_dirty();
    }

    SDL_Rect dest_rect = SDLAdapter::to_sdl_rect(r);
    dest_rect.x *= scale_;
    dest_rect.y *= scale_;
    dest_rect.w *= scale_;
    dest_rect.h *= scale_;

    bool rerender = view->is_dirty();
    bool recomposite = rerender || below_changed;

    if (rerender) {
      if (!layer.preserve_background) {
        layer_surface->fill(0);
        layer_surface->clear_palette_state();
      }

      RenderContext ctx{layer_surface.get()};
      view->render(ctx);
      update_cache(view, *layer_surface);
      view->clean();
    }
    if (recomposite) {
      auto it = cache_.find(view);
      if (it == cache_.end()) {
        if (!layer_surface) {
          layer_surface = std::make_unique<Surface>(width, height);
        }
        if (!layer.preserve_background) {
          layer_surface->fill(0);
          layer_surface->clear_palette_state();
        }
        RenderContext ctx{layer_surface.get()};
        view->render(ctx);

        update_cache(view, *layer_surface);
        it = cache_.find(view);
      }

      SDL_BlitSurface(it->second.get(), nullptr, backbuffer_.get(), &dest_rect);

      frame_changed = true;
      below_changed |= rerender;
    }
  }

  force_recompose_ = false;

  // Software cursor: the cursor can move while no layer changed, so we
  // have to re-present in that case. Also if the cursor just
  // disappeared we need one more present to erase it.
  const Surface* cursor_sprite =
      cursor_ ? cursor_->software_cursor_sprite() : nullptr;
  const ScreenPoint cursor_pos =
      cursor_ ? cursor_->cursor_position() : ScreenPoint{};
  const bool cursor_changed =
      (cursor_sprite != last_cursor_sprite_) ||
      (cursor_sprite && (cursor_pos.x != last_cursor_pos_.x ||
                         cursor_pos.y != last_cursor_pos_.y));

  if (!frame_changed && !cursor_changed) {
    return;
  }

  // Composite the cursor onto a copy so the persistent backbuffer stays
  // cursor-free.
  SDL_Surface* composited = backbuffer_.get();
  if (cursor_sprite) {
    const bool recreated =
        ensure_present_surface(backbuffer_->w, backbuffer_->h);
    // If only the cursor changed, frame_with_cursor_ still holds the
    // current frame and just the old cursor rect needs restoring.
    const bool cursor_only =
        !frame_changed && !recreated && last_cursor_sprite_;
    SDL_SetSurfaceBlendMode(backbuffer_.get(), SDL_BLENDMODE_NONE);
    if (cursor_only) {
      SDL_BlitSurface(backbuffer_.get(), &last_cursor_rect_,
                      frame_with_cursor_.get(), &last_cursor_rect_);
    }
    else {
      SDL_BlitSurface(backbuffer_.get(), nullptr, frame_with_cursor_.get(),
                      nullptr);
    }
    SDL_SetSurfaceBlendMode(backbuffer_.get(), SDL_BLENDMODE_BLEND);
    last_cursor_rect_ =
        draw_software_cursor(frame_with_cursor_.get(), *cursor_sprite,
                             cursor_pos, cursor_->software_cursor_centered());
    composited = frame_with_cursor_.get();
  }
  last_cursor_sprite_ = cursor_sprite;
  last_cursor_pos_ = cursor_pos;

  SDL_Surface* final_surface = composited;

  if (software_crt()) {
    final_surface = apply_crt_scaling(composited);
  }

  SDL_UpdateTexture(sdl_.texture.get(), nullptr, final_surface->pixels,
                    final_surface->pitch);
  SDL_RenderClear(sdl_.renderer.get());
  SDL_RenderTexture(sdl_.renderer.get(), sdl_.texture.get(), nullptr, nullptr);
  SDL_RenderPresent(sdl_.renderer.get());

  if (fade_state_ == FadeState::FADING_OUT && fade_factor_ <= 0.0f) {
    fade_state_ = FadeState::FADED_OUT;
  }
}

bool Renderer::ensure_present_surface(int w, int h) {
  if (frame_with_cursor_ && frame_with_cursor_->w == w &&
      frame_with_cursor_->h == h) {
    return false;
  }
  frame_with_cursor_.reset(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888));
  if (!frame_with_cursor_) {
    fatal(SDL_GetError());
  }
  return true;
}

SDL_Rect Renderer::draw_software_cursor(SDL_Surface* dst, const Surface& sprite,
                                        ScreenPoint pos, bool centered) const {
  SDL_Surface_unique_ptr sdl_sprite = to_sdl_surface(sprite, 1.0f, scale_);

  // The logical display height may differ from SCREEN_HEIGHT due to CRT
  // correction, so just the y coord has to be rescaled.
  int x = pos.x * scale_;
  int y = pos.y * DisplayConstants::SCREEN_HEIGHT / display_height_ * scale_;
  if (centered) {
    x -= sdl_sprite->w / 2;
    y -= sdl_sprite->h / 2;
  }

  SDL_Rect dest = {x, y, sdl_sprite->w, sdl_sprite->h};
  SDL_BlitSurface(sdl_sprite.get(), nullptr, dst, &dest);
  return dest;
}

void Renderer::clear_layers() {
  layers_.clear();
  cache_.clear();
  force_recompose_ = true;
  if (backbuffer_) {
    SDL_FillSurfaceRect(backbuffer_.get(), nullptr,
                        SDL_MapSurfaceRGB(backbuffer_.get(), 0, 0, 0));
  }
}

void Renderer::clear_layers_preserve_backbuffer() {
  layers_.clear();
  cache_.clear();
  force_recompose_ = true;
}

void Renderer::update_cache(Renderable* layer, const Surface& surface) {
  cache_[layer] = to_sdl_surface(
      surface, layer->ignore_fade() ? 1.0f : fade_factor_, scale_);
}

int Renderer::frame_width() const {
  return DisplayConstants::SCREEN_WIDTH * scale_;
}

int Renderer::frame_height() const {
  return DisplayConstants::SCREEN_HEIGHT * scale_;
}

// In HD the GPU stretches the frame to the CRT aspect ratio when it is
// presented (see Game::create_display_texture), so the software pass that
// works on the 640x400 frame is only for the original resolution.
bool Renderer::software_crt() const {
  return scale_ == 1 && display_height_ == DisplayConstants::CRT_DISPLAY_HEIGHT;
}

void Renderer::ensure_backbuffer(int w, int h) {
  if (backbuffer_ && backbuffer_->w == w && backbuffer_->h == h) {
    return;
  }

  backbuffer_.reset(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888));

  if (!backbuffer_) {
    fatal(SDL_GetError());
  }
}

SDL_Surface_unique_ptr Renderer::to_sdl_surface(const Surface& src,
                                                float fade_factor, int scale) {
  const int w = src.width();
  const int h = src.height();

  if (scale > 1) {
    const int big_w = w * scale;
    SDL_Surface* rgba =
        SDL_CreateSurface(big_w, h * scale, SDL_PIXELFORMAT_RGBA8888);
    if (!rgba) {
      fatal(SDL_GetError());
    }
    // Index 0 is always the transparent colour key.
    std::array<uint32_t, 256> lut{};
    lut.fill(gfx::pack_rgba(0, 0, 0, 255));
    const auto& pal = src.colors();
    for (std::size_t i = 0; i < pal.size() && i < lut.size(); ++i) {
      lut[i] = gfx::pack_rgba(
          static_cast<uint32_t>(static_cast<float>(pal[i].r) * fade_factor),
          static_cast<uint32_t>(static_cast<float>(pal[i].g) * fade_factor),
          static_cast<uint32_t>(static_cast<float>(pal[i].b) * fade_factor),
          255);
    }
    lut[0] = 0;

    const gfx::HdImage* hd = src.hd();
    const uint8_t* index = src.pixels().data();
    SDL_LockSurface(rgba);
    auto* base = static_cast<uint8_t*>(rgba->pixels);
    const int pitch = rgba->pitch;
    const bool full_brightness = fade_factor >= 1.0f;

    // Rows are independent, so big frames are split across threads.
    auto expand_rows = [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) {
        auto* first = reinterpret_cast<uint32_t*>(
            base + static_cast<std::ptrdiff_t>(y * scale) * pitch);
        const uint8_t* src_row = index + static_cast<std::ptrdiff_t>(y) * w;
        for (int x = 0; x < w; ++x) {
          std::fill_n(first + x * scale, scale, lut[src_row[x]]);
        }
        for (int ty = 0; ty < scale; ++ty) {
          auto* out = reinterpret_cast<uint32_t*>(
              base + static_cast<std::ptrdiff_t>(y * scale + ty) * pitch);
          if (ty > 0) {
            std::memcpy(out, first,
                        static_cast<std::size_t>(big_w) * sizeof(uint32_t));
          }
          if (!hd) {
            continue;
          }
          const uint32_t* texels = hd->row(y * scale + ty);
          for (int tx = 0; tx < big_w; ++tx) {
            const uint32_t t = texels[tx];
            const uint32_t a = gfx::alpha(t);
            if (a == 255) {
              out[tx] = full_brightness ? t : gfx::scale_rgb(t, fade_factor);
            }
            else if (a != 0) {
              out[tx] = gfx::blend_over(
                  full_brightness ? t : gfx::scale_rgb(t, fade_factor),
                  out[tx]);
            }
          }
        }
      }
    };

    const auto texels = static_cast<std::size_t>(big_w) * h * scale;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int workers =
        texels < 1'000'000
            ? 1
            : static_cast<int>(std::min({hw, 8u, static_cast<unsigned>(h)}));
    if (workers <= 1) {
      expand_rows(0, h);
    }
    else {
      std::vector<std::jthread> pool;
      pool.reserve(static_cast<std::size_t>(workers - 1));
      const int step = (h + workers - 1) / workers;
      for (int i = 1; i < workers; ++i) {
        const int y0 = std::min(h, i * step);
        const int y1 = std::min(h, y0 + step);
        if (y0 < y1) {
          pool.emplace_back([&expand_rows, y0, y1] { expand_rows(y0, y1); });
        }
      }
      expand_rows(0, std::min(h, step));
    } // jthreads join here
    SDL_UnlockSurface(rgba);
    SDL_SetSurfaceBlendMode(rgba, SDL_BLENDMODE_BLEND);
    return SDL_Surface_unique_ptr(rgba);
  }

  SDL_Surface* index8 = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_INDEX8);
  if (!index8) {
    fatal(SDL_GetError());
  }

  SDL_Palette* palette = SDL_CreateSurfacePalette(index8);
  if (!palette) {
    fatal(SDL_GetError());
  }

  // Build palette with fade applied (1.0 = normal, 0.0 = black)
  SDL_Color sdlPal[256];
  const auto pal = src.colors();
  for (size_t i = 0; i < pal.size(); ++i) {
    sdlPal[i] = {static_cast<uint8_t>(pal[i].r * fade_factor),
                 static_cast<uint8_t>(pal[i].g * fade_factor),
                 static_cast<uint8_t>(pal[i].b * fade_factor),
                 SDL_ALPHA_OPAQUE};
  }

  SDL_SetPaletteColors(palette, sdlPal, 0, static_cast<int>(pal.size()));
  SDL_SetSurfaceColorKey(index8, true, 0);
  SDL_SetSurfaceRLE(index8, true);

  SDL_LockSurface(index8);
  uint8_t* dst = static_cast<uint8_t*>(index8->pixels);
  const uint8_t* raw_src = src.pixels().data();

  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + y * index8->pitch, raw_src + y * w, w);
  }
  SDL_UnlockSurface(index8);

  // SDL does the palette to RGBA expansion
  SDL_Surface* rgba = SDL_ConvertSurface(index8, SDL_PIXELFORMAT_RGBA8888);
  SDL_DestroySurface(index8);

  if (!rgba) {
    fatal(SDL_GetError());
  }
  return SDL_Surface_unique_ptr(rgba);
}

void Renderer::with_current_frame(
    std::function<void(SDL_Surface*)> callback) const {
  if (software_crt()) {
    callback(apply_crt_scaling(backbuffer_.get()));
  }
  else {
    callback(backbuffer_.get());
  }
}

SDL_Surface* Renderer::apply_crt_scaling(SDL_Surface* source) const {
  // 640x400 -> 640x1200 (nearest) -> 640x480 (bilinear)
  // Thanks to https://www.youtube.com/watch?v=YvckyWxHAIw (DOS Gaming Aspect
  // Ratio - 320x200) which talks about why this is a good way to do it
  const int INTERMEDIATE_HEIGHT = DisplayConstants::LOGICAL_HEIGHT * 3;
  const int source_width = source->w;
  const int source_height = source->h;

  if (!crt_intermediate_ || crt_intermediate_->w != source_width ||
      crt_intermediate_->h != INTERMEDIATE_HEIGHT) {
    crt_intermediate_.reset(SDL_CreateSurface(source_width, INTERMEDIATE_HEIGHT,
                                              SDL_PIXELFORMAT_RGBA8888));
    if (!crt_intermediate_) {
      fatal(SDL_GetError());
    }
  }
  if (!crt_scaled_ || crt_scaled_->w != source_width ||
      crt_scaled_->h != display_height_) {
    crt_scaled_.reset(SDL_CreateSurface(source_width, display_height_,
                                        SDL_PIXELFORMAT_RGBA8888));
    if (!crt_scaled_) {
      fatal(SDL_GetError());
    }
  }

  // Stage 1: nearest-neighbor 3x vertical upscale
  SDL_LockSurface(source);
  SDL_LockSurface(crt_intermediate_.get());

  const auto* src_pixels = static_cast<const uint8_t*>(source->pixels);
  auto* int_pixels = static_cast<uint8_t*>(crt_intermediate_->pixels);
  const std::size_t row_bytes = static_cast<std::size_t>(source_width) * 4;

  for (int y = 0; y < source_height; ++y) {
    const uint8_t* src_row = src_pixels + (y * source->pitch);
    for (int dy = 0; dy < 3; ++dy) {
      std::memcpy(int_pixels + (((y * 3) + dy) * crt_intermediate_->pitch),
                  src_row, row_bytes);
    }
  }

  SDL_UnlockSurface(crt_intermediate_.get());
  SDL_UnlockSurface(source);

  // Stage 2: bilinear downscale to display height
  SDL_Rect int_rect = {0, 0, source_width, INTERMEDIATE_HEIGHT};
  SDL_Rect final_rect = {0, 0, source_width, display_height_};
  SDL_StretchSurface(crt_intermediate_.get(), &int_rect, crt_scaled_.get(),
                     &final_rect, SDL_SCALEMODE_LINEAR);

  return crt_scaled_.get();
}

bool Renderer::tick_fade() {
  if (fade_state_ == FadeState::NONE || fade_state_ == FadeState::FADED_OUT) {
    return false;
  }

  fade_elapsed_frames_++;

  if (fade_elapsed_frames_ >= fade_duration_frames_) {
    if (fade_state_ == FadeState::FADING_OUT) {
      fade_factor_ = 0.0f;
    }
    else if (fade_state_ == FadeState::FADING_IN) {
      fade_factor_ = 1.0f;
      fade_state_ = FadeState::NONE;
    }
    force_recompose_ = true;
    return true;
  }

  float progress = static_cast<float>(fade_elapsed_frames_) /
                   static_cast<float>(fade_duration_frames_);

  if (fade_state_ == FadeState::FADING_OUT) {
    fade_factor_ = 1.0f - progress;
  }
  else if (fade_state_ == FadeState::FADING_IN) {
    fade_factor_ = progress;
  }

  force_recompose_ = true;
  return false;
}

void Renderer::start_fade_out(int duration_frames) {
  fade_state_ = FadeState::FADING_OUT;
  fade_duration_frames_ = duration_frames;
  fade_elapsed_frames_ = 0;
  fade_factor_ = 1.0f;
  force_recompose_ = true;
}

void Renderer::start_fade_in(int duration_frames) {
  fade_state_ = FadeState::FADING_IN;
  fade_duration_frames_ = duration_frames;
  fade_elapsed_frames_ = 0;
  fade_factor_ = 0.0f;
  force_recompose_ = true;
}

bool Renderer::is_fading() const { return fade_state_ != FadeState::NONE; }

bool Renderer::fade_out_complete() const {
  return fade_state_ == FadeState::FADED_OUT;
}
