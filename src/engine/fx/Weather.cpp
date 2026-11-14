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

void Snowfall::respawn(Flake& f, bool anywhere) {
  const float k = static_cast<float>(h_) / 400.0f; // texels per engine pixel
  // Near flakes are bigger, faster and brighter.
  const float depth = next();
  f.size = k * (0.9f + 2.1f * depth);
  f.speed = k * (14.0f + 30.0f * depth) * (reduced_ ? 0.33f : 1.0f);
  f.sway = reduced_ ? 0.0f : k * (1.0f + 3.0f * next());
  f.phase = next() * 6.2831853f;
  f.alpha = 0.45f + 0.5f * depth;
  f.x = next() * static_cast<float>(w_);
  f.y = anywhere ? next() * static_cast<float>(h_) : -f.size;
}

void Snowfall::configure(Weather weather, bool reduced_motion, int w, int h) {
  if (weather == weather_ && reduced_motion == reduced_ && w == w_ && h == h_) {
    return;
  }
  weather_ = weather;
  reduced_ = reduced_motion;
  w_ = w;
  h_ = h;
  time_ = 0.0f;
  state_ = seed_ ? seed_ : 1u;
  flakes_.assign(flake_count(weather, reduced_motion, w, h), Flake{});
  for (Flake& f : flakes_) {
    respawn(f, true);
  }
}

void Snowfall::step(float seconds) {
  const float dt = std::clamp(seconds, 0.0f, 0.1f);
  time_ += dt;
  // Keep the clock small so sin() stays precise over a long session.
  if (time_ > 3600.0f) {
    time_ -= 3600.0f;
  }
  for (Flake& f : flakes_) {
    f.y += f.speed * dt;
    if (f.y - f.size > static_cast<float>(h_)) {
      respawn(f, false);
    }
  }
}

float Snowfall::drawn_x(const Flake& f) const {
  return f.x + f.sway * std::sin(f.phase + time_ * 1.3f);
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
