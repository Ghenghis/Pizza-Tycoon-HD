/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/Weather.h"

#include <algorithm>
#include <cmath>
#include "engine/Fatal.h"

namespace fx {

std::size_t Snowfall::flake_count(Weather weather, bool reduced_motion, int w,
                                  int h) {
  // Bounded whatever the settings.
  if (weather == Weather::None || w <= 0 || h <= 0) {
    return 0;
  }
  // The same number at any resolution: the flakes scale with the frame.
  const std::size_t base = weather == Weather::Snow ? 520 : 170;
  const std::size_t n = reduced_motion ? base / 3 : base;
  return std::min(n, max_flakes);
}

float Snowfall::next() {
  // xorshift32: the snow's own numbers, never the game's.
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return static_cast<float>(state_ >> 8) / static_cast<float>(1u << 24);
}

void Snowfall::spawn(Flake& f) {
  const float k = static_cast<float>(h_) / 400.0f; // texels per engine pixel
  // Near flakes are bigger, faster and brighter.
  const float depth = next();
  f.size = k * (0.9f + 2.1f * depth);
  f.speed = k * (14.0f + 30.0f * depth) * (reduced_ ? 0.33f : 1.0f);
  f.sway = reduced_ ? 0.0f : k * (1.0f + 3.0f * next());
  f.phase = next() * 6.2831853f;
  f.alpha = 0.45f + 0.5f * depth;
  f.x0 = next() * static_cast<float>(w_);
  f.y0 = next() * (static_cast<float>(h_) + 2.0f * f.size);
  f.x = f.x0;
  f.y = f.y0 - f.size;
}

void Snowfall::configure(Weather weather, bool reduced_motion, int w, int h) {
  if (weather == weather_ && reduced_motion == reduced_ && w == w_ && h == h_) {
    return;
  }
  weather_ = weather;
  reduced_ = reduced_motion;
  w_ = w;
  h_ = h;
  time_ = 0.0;
  state_ = seed_ ? seed_ : 1u;
  flakes_.assign(flake_count(weather, reduced_motion, w, h), Flake{});
  for (Flake& f : flakes_) {
    spawn(f);
  }
  set_time(0.0);
}

void Snowfall::set_time(double seconds) {
  time_ = std::max(0.0, seconds);
  const auto w = static_cast<double>(w_);
  for (Flake& f : flakes_) {
    // Falling through a loop of the frame's height plus the flake.
    const double span = static_cast<double>(h_) + 2.0 * f.size;
    const double pos = static_cast<double>(f.y0) + f.speed * time_;
    const double laps = std::floor(pos / span);
    f.y = static_cast<float>(pos - laps * span) - f.size;
    // Each lap at another place across (golden-ratio steps).
    const double across = static_cast<double>(f.x0) / std::max(w, 1.0) +
                          laps * 0.6180339887498949;
    f.x = static_cast<float>((across - std::floor(across)) * w);
  }
}

void Snowfall::step(float seconds) {
  set_time(time_ + static_cast<double>(std::clamp(seconds, 0.0f, 0.1f)));
}

float Snowfall::drawn_x(const Flake& f) const {
  // The sway's angle wrapped to one turn, so it stays precise however long
  // the game runs.
  const double turn = std::fmod(time_ * 1.3, 6.283185307179586);
  return f.x + f.sway * static_cast<float>(
                            std::sin(static_cast<double>(f.phase) + turn));
}

void Snowfall::build(std::vector<SDL_Vertex>& vertices,
                     std::vector<int>& indices) const {
  vertices.clear();
  indices.clear();
  vertices.reserve(flakes_.size() * 4);
  indices.reserve(flakes_.size() * 6);
  for (const Flake& f : flakes_) {
    const float x = drawn_x(f);
    const float r = f.size * 0.5f;
    const SDL_FColor c{f.alpha, f.alpha, f.alpha, f.alpha};
    const auto first = static_cast<int>(vertices.size());
    vertices.push_back({{x - r, f.y - r}, c, {0.0f, 0.0f}});
    vertices.push_back({{x + r, f.y - r}, c, {1.0f, 0.0f}});
    vertices.push_back({{x + r, f.y + r}, c, {1.0f, 1.0f}});
    vertices.push_back({{x - r, f.y + r}, c, {0.0f, 1.0f}});
    for (int v : {0, 1, 2, 0, 2, 3}) {
      indices.push_back(first + v);
    }
  }
}

SnowLayer::~SnowLayer() {
  if (flake_) {
    SDL_DestroyTexture(flake_);
  }
}

void SnowLayer::on_render_reset() {
  if (flake_) {
    SDL_DestroyTexture(flake_);
    flake_ = nullptr;
  }
}

SDL_Texture* SnowLayer::flake_texture() {
  if (flake_) {
    return flake_;
  }
  // A soft white dot, premultiplied.
  constexpr int size = 16;
  std::vector<uint32_t> px(static_cast<std::size_t>(size) * size);
  const float c = (size - 1) / 2.0f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (static_cast<float>(x) - c) / c;
      const float dy = (static_cast<float>(y) - c) / c;
      const float d2 = std::min(1.0f, dx * dx + dy * dy);
      const auto v =
          static_cast<uint32_t>(std::lround((1.0f - d2) * (1.0f - d2) * 255));
      px[static_cast<std::size_t>(y) * size + x] =
          (v << 24) | (v << 16) | (v << 8) | v;
    }
  }
  flake_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                             SDL_TEXTUREACCESS_STATIC, size, size);
  if (!flake_) {
    fatal(SDL_GetError());
  }
  SDL_UpdateTexture(flake_, nullptr, px.data(), size * 4);
  SDL_SetTextureScaleMode(flake_, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(flake_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  return flake_;
}

bool SnowLayer::draw(const SDL_Rect* clip) {
  if (snow_.flakes().empty()) {
    return false;
  }
  snow_.build(vertices_, indices_);
  if (clip) {
    SDL_SetRenderClipRect(renderer_, clip);
  }
  SDL_RenderGeometry(renderer_, flake_texture(), vertices_.data(),
                     static_cast<int>(vertices_.size()), indices_.data(),
                     static_cast<int>(indices_.size()));
  if (clip) {
    SDL_SetRenderClipRect(renderer_, nullptr);
  }
  return true;
}

} // namespace fx
