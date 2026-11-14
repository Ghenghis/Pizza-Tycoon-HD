/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "engine/HdCompositor.h"
#include "engine/LayerInfo.h"
#include "engine/LightPass.h"
#include "engine/SDLContext.h"
#include "engine/fx/NormalLight.h"
#include "engine/fx/PostFx.h"
#include "engine/fx/Weather.h"
#include "engine/geom/Spaces.h"
#include "engine/gfx/Blit.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Surface.h"

class Renderable;
class CursorManager;

class Renderer {
public:
  enum class FadeState { NONE, FADING_OUT, FADED_OUT, FADING_IN };

  Renderer(const SDLContext& sdl, int display_height)
      : sdl_(sdl), display_height_(display_height), scale_(gfx::hd_scale()) {}
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  void add_layers(Renderable* r);
  void render();
  void clear_layers();
  void clear_layers_preserve_backbuffer();
  // Expands an indexed surface to RGBA. With scale > 1 every pixel becomes
  // scale x scale texels and the surface's HD texels are laid over them.
  static SDL_Surface_unique_ptr to_sdl_surface(const Surface& surface,
                                               float fade_factor = 1.0f,
                                               int scale = 1);
  // Size of the frame this renderer produces, in texels.
  [[nodiscard]] int frame_width() const;
  [[nodiscard]] int frame_height() const;
  void with_current_frame(std::function<void(SDL_Surface*)> callback) const;
  void set_display_height(int height) { display_height_ = height; }
  // Recomposite and present even when no layer is dirty.
  void invalidate() { force_recompose_ = true; }
  // Marks every layer for a full redraw (benchmarks).
  void redraw_all();
  // Uploads everything the HD pack has pre-decoded, now, and creates the
  // render targets and light maps the current layers draw into (benchmarks:
  // in the game the HD art streams in a little each frame, and the targets
  // are reused from the screen before).
  void warm_up_gpu();
  void set_cursor_manager(CursorManager* cursor) { cursor_ = cursor; }
  // SDL_EVENT_RENDER_TARGETS_RESET (false) or SDL_EVENT_RENDER_DEVICE_RESET
  // (true, the display texture is made again by the caller first).
  void on_render_reset(bool device_lost);
  // HD only, display only: the original 1994 look everywhere, or left of a
  // divider at engine x divider_x (the two side by side). Nothing in the
  // game changes; the HD text and lighting switches are the caller's.
  void classic_look(bool on);
  void compare(std::optional<int> divider_x);

  // Fade control - returns true when fade completes
  bool tick_fade();
  void start_fade_out(int duration_frames);
  void start_fade_in(int duration_frames);
  [[nodiscard]] bool is_fading() const;
  [[nodiscard]] bool fade_out_complete() const;

  // How the last frame went to the window (fx::PostFx): plainly, through
  // the post-FX shader, or with fixed-function colour modulation.
  [[nodiscard]] fx::PostFx::Path last_post_path() const { return post_path_; }
  // True when the post-FX shader runs (SDL's GPU render driver).
  [[nodiscard]] bool post_fx_shader();

  // GPU statistics of the last frame (HD only).
  [[nodiscard]] const HdCompositor* compositor() const { return gpu_.get(); }
  // Where the last HD frame's time went, on the CPU side: drawing the views
  // (their indexed pixels and draw lists), turning those into GPU commands
  // and uploads, and handing the frame to the display.
  struct FrameTimes {
    double views_ms = 0;
    double compose_ms = 0;
    double present_ms = 0;
  };
  [[nodiscard]] const FrameTimes& frame_times() const { return times_; }

private:
  // HD: layers are drawn by the GPU (HdCompositor) into textures and
  // composited there; the CPU only expands 640x400 indexed pixels.
  void render_gpu();
  // Streams pre-decoded HD art to the GPU for up to budget_ms.
  void stream_ahead(double budget_ms);
  // Per frame: small enough not to be felt at 144 Hz.
  static constexpr double stream_budget_ms = 3.0;
  SDL_Texture* layer_texture(Renderable* view, int w, int h);
  // The lit copy of a layer drawn into `albedo`.
  SDL_Texture* lit_texture(Renderable* view, SDL_Texture* albedo);
  // Render targets are kept for the next screen: creating a 4K target costs
  // milliseconds, and most screens have the same layer sizes.
  SDL_Texture* take_target(int w, int h);
  void give_back(SDL_Texture* tex);
  void ensure_back_texture();
  void clear_back_texture();
  void destroy_layer_textures();
  void update_cache(Renderable* layer, const gfx::Surface& source_surface);
  void ensure_backbuffer(int w, int h);
  // Returns true when the surface was (re)created.
  bool ensure_present_surface(int w, int h);
  // Returns the rect the cursor was drawn into.
  SDL_Rect draw_software_cursor(SDL_Surface* dst, const gfx::Surface& sprite,
                                ScreenPoint pos, bool centered) const;
  SDL_Surface* apply_crt_scaling(SDL_Surface* source) const;

  std::unordered_map<Renderable*, SDL_Surface_unique_ptr> cache_;
  SDL_Surface_unique_ptr backbuffer_;
  // Copy with the software cursor on top
  SDL_Surface_unique_ptr frame_with_cursor_;
  // Reused by apply_crt_scaling
  mutable SDL_Surface_unique_ptr crt_intermediate_;
  mutable SDL_Surface_unique_ptr crt_scaled_;
  CursorManager* cursor_{nullptr};
  // This allows us to re-present when only the cursor changed.
  const gfx::Surface* last_cursor_sprite_{nullptr};
  ScreenPoint last_cursor_pos_{};
  // The software cursor's last position in frame texels (HD, see
  // render_gpu).
  float last_cursor_x_ = -1.0f;
  float last_cursor_y_ = -1.0f;
  SDL_Rect last_cursor_rect_{};
  bool force_recompose_ = false;
  std::unordered_map<Renderable*, std::unique_ptr<Surface>> layer_surfaces_;
  std::vector<LayerInfo> layers_;
  const SDLContext& sdl_;
  int display_height_;
  // HD texels per engine pixel (gfx::hd_scale() when constructed).
  int scale_;
  [[nodiscard]] bool software_crt() const;

  void draw_compare_divider();
  // The picture settings for this frame (fx::frame_state(), or nothing at
  // all in the 1994 look).
  [[nodiscard]] fx::FrameState frame_state() const;
  [[nodiscard]] fx::Grade frame_grade() const { return frame_state().grade; }
  [[nodiscard]] LightPass::Strength light_strength() const;
  // Draws the display texture to the window through the picture pass.
  void present_display_texture();
  std::unique_ptr<fx::PostFx> post_;
  fx::PostFx::Path post_path_ = fx::PostFx::Path::Plain;
  // The picture settings of the last frame: a change redraws the frame.
  fx::FrameState last_state_;
  // Night glow of emissive art, per layer (HdCompositor::draw_emissive),
  // and the layers whose glow is up to date.
  std::unordered_map<Renderable*, SDL_Texture*> glow_tex_;
  std::unordered_set<Renderable*> glow_ready_;
  // Adds the layer's night glow onto the current target at `dest`.
  void add_glow(Renderable* view, const Surface& surface, const SDL_FRect& dest,
                float strength);
  // Normal-map lighting (SDL's GPU driver, pictures with normal maps).
  std::unique_ptr<fx::NormalLightPass> normal_;
  fx::NormalLights normal_lights_;
  // Weather over the frame.
  std::unique_ptr<fx::SnowLayer> snow_;
  uint64_t snow_ticks_ = 0;
  bool classic_ = false;
  std::optional<int> compare_x_;
  std::unique_ptr<HdCompositor> gpu_;
  FrameTimes times_;
  std::unique_ptr<LightPass> light_;
  // Lit copy of each lit layer (see Renderable::lighting).
  std::unordered_map<Renderable*, SDL_Texture*> lit_tex_;
  std::unordered_map<Renderable*, SDL_Texture*> layer_tex_;
  std::vector<SDL_Texture*> target_pool_;
  // A screen has up to a dozen layers (the view, its panels, the overlays).
  static constexpr std::size_t max_pooled_targets = 12;
  std::unordered_set<Renderable*> gpu_cached_;
  SDL_Texture* back_tex_{nullptr};
  SDL_Texture* cursor_tex_{nullptr};
  const Surface* cursor_tex_sprite_{nullptr};

  FadeState fade_state_{FadeState::NONE};
  float fade_factor_{1.0f}; // 1.0 = normal, 0.0 = black
  int fade_duration_frames_{0};
  int fade_elapsed_frames_{0};
};
