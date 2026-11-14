/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <thread>
#include <vector>
#include "engine/DisplayConstants.h"
#include "engine/Fatal.h"
#include "engine/Renderable.h"
#include "engine/SDL_include.h"
#include "engine/adapters/SDLRectAdapter.h"
#include "engine/fx/DayNight.h"
#include "engine/fx/Look.h"
#include "engine/fx/MapStore.h"
#include "engine/gfx/Surface.h"
#include "engine/ui/CursorManager.h"
#include "resources/HdPack.h"
#include "resources/HdTypeface.h"

using gfx::Surface;

void Renderer::add_layers(Renderable* r) {
  auto layers = r->get_layers();
  for (const auto& layer : layers) {
    layers_.push_back(layer);
  }
}

Renderer::~Renderer() {
  destroy_layer_textures();
  for (SDL_Texture* tex : target_pool_) {
    SDL_DestroyTexture(tex);
  }
  if (back_tex_) {
    SDL_DestroyTexture(back_tex_);
  }
  if (cursor_tex_) {
    SDL_DestroyTexture(cursor_tex_);
  }
}

void Renderer::redraw_all() {
  for (auto& layer : layers_) {
    layer.view->mark_dirty();
  }
  force_recompose_ = true;
}

void Renderer::destroy_layer_textures() {
  for (auto& [view, tex] : layer_tex_) {
    give_back(tex);
  }
  layer_tex_.clear();
  for (auto& [view, tex] : lit_tex_) {
    give_back(tex);
  }
  lit_tex_.clear();
  for (auto& [view, tex] : glow_tex_) {
    give_back(tex);
  }
  glow_tex_.clear();
  glow_ready_.clear();
  gpu_cached_.clear();
}

SDL_Texture* Renderer::take_target(int w, int h) {
  SDL_Texture* tex = nullptr;
  if (auto it = std::ranges::find_if(
          target_pool_,
          [w, h](const SDL_Texture* t) { return t->w == w && t->h == h; });
      it != target_pool_.end()) {
    tex = *it;
    target_pool_.erase(it);
  }
  else {
    tex = SDL_CreateTexture(sdl_.renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_TARGET, w, h);
    if (!tex) {
      fatal(SDL_GetError());
    }
  }
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  SDL_SetTextureColorModFloat(tex, 1.0f, 1.0f, 1.0f);
  SDL_SetTextureAlphaModFloat(tex, 1.0f);
  return tex;
}

void Renderer::give_back(SDL_Texture* tex) {
  if (!tex) {
    return;
  }
  target_pool_.push_back(tex);
  if (target_pool_.size() > max_pooled_targets) {
    SDL_DestroyTexture(target_pool_.front());
    target_pool_.erase(target_pool_.begin());
  }
}

SDL_Texture* Renderer::layer_texture(Renderable* view, int w, int h) {
  SDL_Texture*& tex = layer_tex_[view];
  if (tex && (tex->w != w || tex->h != h)) {
    give_back(tex);
    tex = nullptr;
  }
  if (!tex) {
    tex = take_target(w, h);
    gpu_cached_.erase(view);
  }
  return tex;
}

SDL_Texture* Renderer::lit_texture(Renderable* view, SDL_Texture* albedo) {
  SDL_Texture*& out = lit_tex_[view];
  if (out && (out->w != albedo->w || out->h != albedo->h)) {
    give_back(out);
    out = nullptr;
  }
  if (!out) {
    out = take_target(albedo->w, albedo->h);
  }
  return out;
}

void Renderer::ensure_back_texture() {
  if (back_tex_) {
    return;
  }
  back_tex_ = SDL_CreateTexture(sdl_.renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET, frame_width(),
                                frame_height());
  if (!back_tex_) {
    fatal(SDL_GetError());
  }
  SDL_SetTextureBlendMode(back_tex_, SDL_BLENDMODE_NONE);
  SDL_SetTextureScaleMode(back_tex_, SDL_SCALEMODE_NEAREST);
  clear_back_texture();
}

void Renderer::clear_back_texture() {
  if (!back_tex_) {
    return;
  }
  SDL_Renderer* r = sdl_.renderer.get();
  SDL_SetRenderTarget(r, back_tex_);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  SDL_SetRenderTarget(r, nullptr);
}

void Renderer::stream_ahead(double budget_ms) {
  if (!gpu_) {
    gpu_ = std::make_unique<HdCompositor>(sdl_.renderer.get());
  }
  auto add = resources::HdPack::instance().take_prefetched();
  // The HD text's glyph sheets, as soon as they are made.
  for (auto& sheet : resources::HdTypefaces::instance().take_new_sheets()) {
    add.push_back(std::move(sheet));
  }
  gpu_->stream(add, budget_ms);
}

void Renderer::classic_look(bool on) {
  if (classic_ != on) {
    classic_ = on;
    redraw_all();
  }
}

void Renderer::compare(std::optional<int> divider_x) {
  if (divider_x) {
    *divider_x = std::clamp(*divider_x, 0, DisplayConstants::SCREEN_WIDTH);
  }
  if (compare_x_ != divider_x) {
    // The divider is part of every layer's picture: all are drawn again.
    compare_x_ = divider_x;
    redraw_all();
  }
}

void Renderer::draw_compare_divider() {
  SDL_Renderer* r = sdl_.renderer.get();
  const auto k = static_cast<float>(scale_);
  const float x = static_cast<float>(*compare_x_) * k;
  const auto h = static_cast<float>(frame_height());
  // A light line with a dark edge, readable on any picture.
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
  const SDL_FRect shade{x - k * 0.75f, 0, k * 1.5f, h};
  SDL_RenderFillRect(r, &shade);
  SDL_SetRenderDrawColor(r, 255, 244, 214, 255);
  const SDL_FRect line{x - k * 0.25f, 0, k * 0.5f, h};
  SDL_RenderFillRect(r, &line);
  // Labels: SDL's 8x8 debug font, drawn at the engine pixel size, which
  // suits the 1994 side well enough.
  const float label_scale = std::max(1.0f, k * 0.5f);
  SDL_SetRenderScale(r, label_scale, label_scale);
  const float y = 4.0f * k / label_scale;
  const float lx = x / label_scale;
  SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
  const SDL_FRect left_chip{lx - 44.0f, y - 2.0f, 40.0f, 12.0f};
  const SDL_FRect right_chip{lx + 4.0f, y - 2.0f, 24.0f, 12.0f};
  SDL_RenderFillRect(r, &left_chip);
  SDL_RenderFillRect(r, &right_chip);
  SDL_SetRenderDrawColor(r, 255, 244, 214, 255);
  SDL_RenderDebugText(r, lx - 40.0f, y, "1994");
  SDL_RenderDebugText(r, lx + 8.0f, y, "HD");
  SDL_SetRenderScale(r, 1.0f, 1.0f);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

fx::FrameState Renderer::frame_state() const {
  // The 1994 look is the 1994 picture: no grade, no light strength, no
  // night glow, no weather.
  return classic_ ? fx::FrameState{} : fx::frame_state();
}

void Renderer::add_glow(Renderable* view, const Surface& surface,
                        const SDL_FRect& dest, float strength) {
  SDL_Renderer* r = sdl_.renderer.get();
  if (!glow_ready_.contains(view)) {
    if (!gpu_->has_emissive(surface)) {
      return;
    }
    SDL_Texture*& glow = glow_tex_[view];
    const int w = surface.width() * scale_;
    const int h = surface.height() * scale_;
    if (glow && (glow->w != w || glow->h != h)) {
      give_back(glow);
      glow = nullptr;
    }
    if (!glow) {
      glow = take_target(w, h);
    }
    SDL_Texture* target = SDL_GetRenderTarget(r);
    int art_from_x = 0;
    if (compare_x_) {
      art_from_x = std::max(
          0, (*compare_x_ - static_cast<int>(dest.x) / scale_) * scale_);
    }
    if (!gpu_->draw_emissive(glow, surface, art_from_x)) {
      SDL_SetRenderTarget(r, target);
      return;
    }
    SDL_SetRenderTarget(r, target);
    glow_ready_.insert(view);
  }
  const auto it = glow_tex_.find(view);
  if (it == glow_tex_.end() || !it->second) {
    return;
  }
  SDL_Texture* glow = it->second;
  SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_ADD_PREMULTIPLIED);
  SDL_SetTextureColorModFloat(glow, strength, strength, strength);
  SDL_RenderTexture(r, glow, nullptr, &dest);
  SDL_SetTextureColorModFloat(glow, 1.0f, 1.0f, 1.0f);
  SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
}

LightPass::Strength Renderer::light_strength() const {
  const fx::Grade g = frame_grade();
  return {g.light_strength, g.shadow_depth};
}

bool Renderer::post_fx_shader() {
  if (!post_) {
    post_ = std::make_unique<fx::PostFx>(sdl_.renderer.get());
  }
  return post_->shader_ready();
}

void Renderer::present_display_texture() {
  if (!post_) {
    post_ = std::make_unique<fx::PostFx>(sdl_.renderer.get());
  }
  fx::FrameInfo info;
  if (compare_x_) {
    // The 1994 side of the comparison stays ungraded.
    info.split_u = static_cast<float>(*compare_x_) /
                   static_cast<float>(DisplayConstants::SCREEN_WIDTH);
  }
  info.scan_rows = static_cast<float>(DisplayConstants::SCREEN_HEIGHT);
  info.mask_cols = static_cast<float>(sdl_.texture->w);
  post_path_ = post_->draw(sdl_.texture.get(), frame_grade(), info);
}

void Renderer::on_render_reset(bool device_lost) {
  if (post_) {
    post_->on_render_reset(device_lost);
  }
  glow_ready_.clear();
  if (normal_) {
    normal_->reset();
  }
  if (device_lost) {
    for (auto& [view, tex] : glow_tex_) {
      SDL_DestroyTexture(tex);
    }
    glow_tex_.clear();
    if (snow_) {
      snow_->on_render_reset();
    }
    normal_.reset();
  }
  if (device_lost) {
    // The old device's textures are gone (SDL keeps the handles, without
    // their contents): every one is destroyed here and made again, and its
    // pictures uploaded again, when next needed.
    for (auto* map : {&layer_tex_, &lit_tex_}) {
      for (auto& [view, tex] : *map) {
        SDL_DestroyTexture(tex);
      }
      map->clear();
    }
    for (SDL_Texture* tex : target_pool_) {
      SDL_DestroyTexture(tex);
    }
    target_pool_.clear();
    if (back_tex_) {
      SDL_DestroyTexture(back_tex_);
      back_tex_ = nullptr;
    }
    if (cursor_tex_) {
      SDL_DestroyTexture(cursor_tex_);
      cursor_tex_ = nullptr;
    }
    cursor_tex_sprite_ = nullptr;
    light_.reset();
    gpu_.reset();
  }
  // Render targets lost their contents: everything is drawn again.
  gpu_cached_.clear();
  cache_.clear();
  force_recompose_ = true;
  clear_back_texture();
}

void Renderer::warm_up_gpu() {
  if (scale_ <= 1 || !hd_gpu_compositing()) {
    return;
  }
  stream_ahead(1e9);
  gpu_->prepare_atlas();
  SDL_Renderer* r = sdl_.renderer.get();
  ensure_back_texture();
  for (const auto& layer : layers_) {
    SDL_Texture* tex =
        layer_texture(layer.view, layer.area.w * scale_, layer.area.h * scale_);
    lighting::Scene scene;
    if (lighting::enabled() && layer.view->lighting(scene) && scene.active()) {
      SDL_Texture* out = lit_texture(layer.view, tex);
      if (!light_) {
        light_ = std::make_unique<LightPass>(r);
      }
      // Makes the light pass's own textures; the first frame draws over it.
      light_->apply(tex, out, layer.area.w, layer.area.h, scale_, scene,
                    static_cast<double>(SDL_GetTicks()) / 1000.0,
                    light_strength());
    }
  }
  SDL_SetRenderTarget(r, nullptr);
}

namespace {
double ms_between(uint64_t a, uint64_t b) {
  return static_cast<double>(b - a) * 1000.0 /
         static_cast<double>(SDL_GetPerformanceFrequency());
}
} // namespace

void Renderer::render_gpu() {
  times_ = {};
  SDL_Renderer* r = sdl_.renderer.get();
  // Where the software cursor goes, in frame texels: the game sees the
  // pointer in whole engine pixels, but the picture follows the real mouse
  // between them, so the cursor glides at HD instead of jumping k texels.
  // (Asked before any render target is set: the conversion uses the
  // window's logical presentation.)
  const ScreenPoint game_pos =
      cursor_ ? cursor_->cursor_position() : ScreenPoint{};
  const float y_scale = static_cast<float>(DisplayConstants::SCREEN_HEIGHT) /
                        static_cast<float>(display_height_) *
                        static_cast<float>(scale_);
  float cursor_x = static_cast<float>(game_pos.x * scale_);
  float cursor_y = static_cast<float>(game_pos.y) * y_scale;
  {
    float wx = 0;
    float wy = 0;
    float lx = 0;
    float ly = 0;
    SDL_GetMouseState(&wx, &wy);
    // Only while it agrees with the game's pointer (not when a pad or the
    // keyboard moved that).
    if (SDL_RenderCoordinatesFromWindow(r, wx, wy, &lx, &ly) &&
        std::abs(lx - static_cast<float>(game_pos.x)) < 1.0f &&
        std::abs(ly - static_cast<float>(game_pos.y)) < 1.0f) {
      cursor_x = lx * static_cast<float>(scale_);
      cursor_y = ly * y_scale;
    }
  }
  if (!gpu_) {
    gpu_ = std::make_unique<HdCompositor>(r);
  }
  ensure_back_texture();

  // During a fade every layer is redrawn with the current fade factor.
  if (is_fading()) {
    gpu_cached_.clear();
    force_recompose_ = true;
  }
  // New picture settings: the lit layers are lit again and the frame shown
  // again.
  const fx::FrameState look = frame_state();
  if (look != last_state_) {
    last_state_ = look;
    force_recompose_ = true;
  }
  const LightPass::Strength strength = light_strength();
  // Normal maps and emissive maps of pictures the pack loaded since.
  fx::MapStore& maps = fx::MapStore::instance();
  maps.sync();
  const bool relief =
      maps.enabled() && hd_gpu_compositing() && fx::PostFx::is_gpu_driver(r);
  if (relief && !normal_) {
    normal_ = std::make_unique<fx::NormalLightPass>(r);
  }
  const fx::Sun sun = look.sun_hours >= 0.0f && look.area == fx::Area::City
                          ? fx::sun_at(look.sun_hours)
                          : fx::Sun{};
  for (auto& layer : layers_) {
    force_recompose_ |= layer.view->needs_force_recompose();
  }
  bool below_changed = force_recompose_;
  bool frame_changed = force_recompose_;

  for (auto& layer : layers_) {
    Renderable* view = layer.view;
    const ViewRect area = layer.area;
    auto& layer_surface = layer_surfaces_[view];
    if (!layer_surface || layer_surface->width() != area.w ||
        layer_surface->height() != area.h) {
      layer_surface = std::make_unique<Surface>(area.w, area.h);
      view->mark_dirty();
    }
    SDL_Texture* tex = layer_texture(view, area.w * scale_, area.h * scale_);

    // Lit layers are relit every frame while a light flickers.
    lighting::Scene scene;
    const bool lit =
        lighting::enabled() && view->lighting(scene) && scene.active();
    const bool relight = lit && scene.animated();

    const bool rerender = view->is_dirty() || !gpu_cached_.contains(view);
    const bool recomposite = rerender || below_changed || relight;
    if (rerender) {
      if (!layer.preserve_background) {
        layer_surface->fill(0);
        layer_surface->clear_palette_state();
      }
      RenderContext ctx{layer_surface.get()};
      const uint64_t t0 = SDL_GetPerformanceCounter();
      view->render(ctx);
      const uint64_t t1 = SDL_GetPerformanceCounter();
      glow_ready_.erase(view);
      // Relief from normal maps: the room's lights (or the sun outdoors).
      bool shaped = false;
      if (relief && normal_->ready()) {
        normal_lights_ = fx::normal_lights(
            lit ? scene : lighting::Scene{}, scale_,
            static_cast<double>(SDL_GetTicks()) / 1000.0, sun);
        if (lit || !normal_lights_.empty()) {
          gpu_->normal_lighting(normal_.get(), &normal_lights_);
          shaped = true;
        }
      }
      // The 1994 look, all or left of the divider: no HD art there.
      int art_from_x = 0;
      if (classic_) {
        art_from_x = area.w * scale_;
      }
      else if (compare_x_) {
        art_from_x = std::max(0, (*compare_x_ - area.x) * scale_);
      }
      gpu_->draw_layer(tex, *layer_surface,
                       view->ignore_fade() ? 1.0f : fade_factor_, art_from_x);
      if (shaped) {
        gpu_->normal_lighting(nullptr, nullptr);
      }
      times_.views_ms += ms_between(t0, t1);
      times_.compose_ms += ms_between(t1, SDL_GetPerformanceCounter());
      gpu_cached_.insert(view);
      view->clean();
    }
    if (recomposite) {
      SDL_Texture* shown = tex;
      if (lit) {
        SDL_Texture* out = lit_texture(view, tex);
        if (!light_) {
          light_ = std::make_unique<LightPass>(r);
        }
        light_->apply(tex, out, area.w, area.h, scale_, scene,
                      static_cast<double>(SDL_GetTicks()) / 1000.0, strength);
        shown = out;
      }
      SDL_SetRenderTarget(r, back_tex_);
      const SDL_FRect dest{static_cast<float>(area.x * scale_),
                           static_cast<float>(area.y * scale_),
                           static_cast<float>(area.w * scale_),
                           static_cast<float>(area.h * scale_)};
      if (lit && compare_x_) {
        // Unlit (1994) left of the divider, lit right of it.
        const int split = *compare_x_ * scale_;
        const SDL_Rect left{0, 0, split, frame_height()};
        const SDL_Rect right{split, 0, frame_width() - split, frame_height()};
        SDL_SetRenderClipRect(r, &left);
        SDL_RenderTexture(r, tex, nullptr, &dest);
        SDL_SetRenderClipRect(r, &right);
        SDL_RenderTexture(r, shown, nullptr, &dest);
        SDL_SetRenderClipRect(r, nullptr);
      }
      else {
        SDL_RenderTexture(r, shown, nullptr, &dest);
      }
      // Emissive art glows at night, over the lighting.
      if (look.night > 0.0f) {
        add_glow(view, *layer_surface, dest,
                 look.night * (view->ignore_fade() ? 1.0f : fade_factor_));
      }
      frame_changed = true;
      below_changed |= rerender || relight;
    }
  }
  force_recompose_ = false;

  const Surface* cursor_sprite =
      cursor_ ? cursor_->software_cursor_sprite() : nullptr;
  const bool cursor_changed = (cursor_sprite != last_cursor_sprite_) ||
                              (cursor_sprite && (cursor_x != last_cursor_x_ ||
                                                 cursor_y != last_cursor_y_));
  // Falling weather moves every frame.
  const uint64_t now = SDL_GetTicks();
  const float weather_dt =
      snow_ticks_ ? static_cast<float>(now - snow_ticks_) / 1000.0f : 0.0f;
  snow_ticks_ = now;
  if (look.weather != fx::Weather::None ||
      (snow_ && !snow_->snowfall().flakes().empty())) {
    if (!snow_) {
      snow_ = std::make_unique<fx::SnowLayer>(r);
    }
    snow_->snowfall().configure(look.weather, look.reduced_motion,
                                frame_width(), frame_height());
    snow_->snowfall().step(weather_dt);
    frame_changed = true;
  }
  if (!frame_changed && !cursor_changed) {
    SDL_SetRenderTarget(r, nullptr);
    stream_ahead(stream_budget_ms);
    gpu_->end_frame();
    return;
  }
  last_cursor_sprite_ = cursor_sprite;
  last_cursor_pos_ = game_pos;
  last_cursor_x_ = cursor_x;
  last_cursor_y_ = cursor_y;

  // The frame plus the cursor, in the display texture the GPU stretches to
  // the window.
  SDL_SetRenderTarget(r, sdl_.texture.get());
  SDL_RenderTexture(r, back_tex_, nullptr, nullptr);
  if (snow_) {
    // Not on the 1994 side of the comparison.
    const int split = compare_x_ ? *compare_x_ * scale_ : 0;
    const SDL_Rect right{split, 0, frame_width() - split, frame_height()};
    snow_->draw(compare_x_ ? &right : nullptr);
  }
  if (compare_x_) {
    draw_compare_divider();
  }
  if (cursor_sprite) {
    if (cursor_sprite != cursor_tex_sprite_ || !cursor_tex_) {
      if (cursor_tex_) {
        SDL_DestroyTexture(cursor_tex_);
      }
      SDL_Surface_unique_ptr rgba = CursorManager::smooth_picture(
          *cursor_sprite, cursor_sprite->width() * scale_,
          cursor_sprite->height() * scale_);
      cursor_tex_ = SDL_CreateTextureFromSurface(r, rgba.get());
      if (cursor_tex_) {
        SDL_SetTextureBlendMode(cursor_tex_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(cursor_tex_, SDL_SCALEMODE_NEAREST);
      }
      cursor_tex_sprite_ = cursor_sprite;
    }
    if (cursor_tex_) {
      const float w = static_cast<float>(cursor_tex_->w);
      const float h = static_cast<float>(cursor_tex_->h);
      float x = std::round(cursor_x);
      float y = std::round(cursor_y);
      if (cursor_->software_cursor_centered()) {
        x -= std::floor(w / 2);
        y -= std::floor(h / 2);
      }
      const SDL_FRect dest{x, y, w, h};
      SDL_RenderTexture(r, cursor_tex_, nullptr, &dest);
    }
  }
  SDL_SetRenderTarget(r, nullptr);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  present_display_texture();
  const uint64_t p0 = SDL_GetPerformanceCounter();
  SDL_RenderPresent(r);
  times_.present_ms += ms_between(p0, SDL_GetPerformanceCounter());
  stream_ahead(stream_budget_ms);
  gpu_->end_frame();

  if (fade_state_ == FadeState::FADING_OUT && fade_factor_ <= 0.0f) {
    fade_state_ = FadeState::FADED_OUT;
  }
}

void Renderer::render() {
  if (scale_ > 1 && hd_gpu_compositing()) {
    render_gpu();
    return;
  }
  // For now all layers have to be full screen size
  ensure_backbuffer(frame_width(), frame_height());

  // During fade, clear cache so all layers regenerate with current fade_factor
  if (is_fading()) {
    cache_.clear();
    force_recompose_ = true;
  }
  // New picture settings: the frame is shown again.
  if (const fx::FrameState look = frame_state(); look != last_state_) {
    last_state_ = look;
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
  present_display_texture();
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
  destroy_layer_textures();
  clear_back_texture();
  force_recompose_ = true;
  if (backbuffer_) {
    SDL_FillSurfaceRect(backbuffer_.get(), nullptr,
                        SDL_MapSurfaceRGB(backbuffer_.get(), 0, 0, 0));
  }
}

void Renderer::clear_layers_preserve_backbuffer() {
  layers_.clear();
  cache_.clear();
  destroy_layer_textures();
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
      // One expanded row of indexed pixels, copied under every texel row
      // before that row's HD texels are blended on.
      std::vector<uint32_t> plain(static_cast<std::size_t>(big_w));
      for (int y = y0; y < y1; ++y) {
        const uint8_t* src_row = index + static_cast<std::ptrdiff_t>(y) * w;
        for (int x = 0; x < w; ++x) {
          std::fill_n(plain.data() + x * scale, scale, lut[src_row[x]]);
        }
        for (int ty = 0; ty < scale; ++ty) {
          auto* out = reinterpret_cast<uint32_t*>(
              base + static_cast<std::ptrdiff_t>(y * scale + ty) * pitch);
          std::memcpy(out, plain.data(),
                      static_cast<std::size_t>(big_w) * sizeof(uint32_t));
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
  if (back_tex_) {
    SDL_Renderer* r = sdl_.renderer.get();
    SDL_SetRenderTarget(r, back_tex_);
    SDL_Surface* read = SDL_RenderReadPixels(r, nullptr);
    SDL_SetRenderTarget(r, nullptr);
    if (!read) {
      fatal(SDL_GetError());
    }
    SDL_Surface_unique_ptr frame(
        SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA8888));
    SDL_DestroySurface(read);
    if (!frame) {
      fatal(SDL_GetError());
    }
    callback(frame.get());
    return;
  }
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
