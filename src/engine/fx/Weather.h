/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include <cstdint>
#include <vector>
#include "engine/fx/Look.h"

// Falling snow over the frame, for the picture only. Its random numbers are
// its own (a fixed-seed xorshift), never the game's, so the simulation is
// untouched; the particle count is bounded; all flakes go to the GPU in one
// SDL_RenderGeometry call.
namespace fx {

struct Flake {
  float x = 0.0f; // frame texels, centre (before sway), at time()
  float y = 0.0f;
  float x0 = 0.0f; // where it starts (time 0); the rest follows from time
  float y0 = 0.0f;
  float size = 1.0f;  // texels across
  float speed = 0.0f; // texels per second, down
  float sway = 0.0f;  // texels side to side
  float phase = 0.0f; // radians
  float alpha = 1.0f; // 0..1
};

class Snowfall {
public:
  // At most this many flakes, whatever the frame size.
  static constexpr std::size_t max_flakes = 900;
  static constexpr uint32_t default_seed = 0x5EED1994u;

  explicit Snowfall(uint32_t seed = default_seed) : seed_(seed) {}

  // Sets what falls over a w x h frame (texels; the original 400 rows
  // stretched to h set the scale). A change starts the snowfall afresh.
  // Reduced motion: a third of the flakes, falling at a third of the speed,
  // with no sway.
  void configure(Weather weather, bool reduced_motion, int w, int h);
  // Moves every flake on by `seconds` (clamped to a tenth of a second, so a
  // stall never makes the snow jump).
  void step(float seconds);
  // Puts every flake where it is at `seconds` (game time, fx::FrameState::
  // anim_seconds): a pure function of the time, so the same time gives the
  // same snow. A flake that leaves the bottom comes back at the top, each
  // time at another place across.
  void set_time(double seconds);

  [[nodiscard]] Weather weather() const noexcept { return weather_; }
  [[nodiscard]] const std::vector<Flake>& flakes() const noexcept {
    return flakes_;
  }
  [[nodiscard]] double time() const noexcept { return time_; }
  // Where a flake is drawn now (sway included).
  [[nodiscard]] float drawn_x(const Flake& f) const;

  // Four vertices and six indices per flake (premultiplied colour), in
  // frame texels. Replaces the contents of both vectors.
  void build(std::vector<SDL_Vertex>& vertices,
             std::vector<int>& indices) const;

  // How many flakes a frame of h rows gets.
  [[nodiscard]] static std::size_t
  flake_count(Weather weather, bool reduced_motion, int w, int h);

private:
  float next(); // 0..1
  void spawn(Flake& f);

  uint32_t seed_;
  uint32_t state_ = 1;
  Weather weather_ = Weather::None;
  bool reduced_ = false;
  int w_ = 0;
  int h_ = 0;
  double time_ = 0.0;
  std::vector<Flake> flakes_;
};

// The snowfall plus its flake texture: draws over the current target.
class SnowLayer {
public:
  explicit SnowLayer(SDL_Renderer* renderer) : renderer_(renderer) {}
  ~SnowLayer();
  SnowLayer(const SnowLayer&) = delete;
  SnowLayer& operator=(const SnowLayer&) = delete;

  Snowfall& snowfall() { return snow_; }
  // Draws the flakes over the current target, clipped to `clip` when
  // given. Returns false when there was nothing to draw.
  bool draw(const SDL_Rect* clip = nullptr);
  void on_render_reset();

private:
  SDL_Texture* flake_texture();

  SDL_Renderer* renderer_;
  SDL_Texture* flake_ = nullptr;
  Snowfall snow_;
  std::vector<SDL_Vertex> vertices_;
  std::vector<int> indices_;
};

} // namespace fx
