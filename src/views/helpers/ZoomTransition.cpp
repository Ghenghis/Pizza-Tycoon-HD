/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/helpers/ZoomTransition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include "core/util/Tween.h"
#include "engine/gfx/Surface.h"

namespace views::helpers {

float MapFrame::screen_x(float wx) const noexcept {
  return static_cast<float>(viewport.x) +
         ((wx - static_cast<float>(top_left.x)) / static_cast<float>(zoom));
}

float MapFrame::screen_y(float wy) const noexcept {
  return static_cast<float>(viewport.y) +
         ((wy - static_cast<float>(top_left.y)) / static_cast<float>(zoom));
}

bool MapFrame::contains(ViewPoint p) const noexcept {
  return p.x >= viewport.x && p.y >= viewport.y &&
         p.x < viewport.x + viewport.w && p.y < viewport.y + viewport.h;
}

void ZoomTransition::begin(const MapFrame& from,
                           std::vector<uint8_t> from_pixels, const MapFrame& to,
                           WorldPoint anchor) {
  if (from.zoom <= 0 || to.zoom <= 0 || from.zoom == to.zoom ||
      from_pixels.size() != static_cast<std::size_t>(from.viewport.w) *
                                static_cast<std::size_t>(from.viewport.h)) {
    active_ = false;
    return;
  }
  from_ = from;
  to_ = to;
  from_pixels_ = std::move(from_pixels);
  anchor_ = anchor;
  from_ax_ = from.screen_x(static_cast<float>(anchor.x));
  from_ay_ = from.screen_y(static_cast<float>(anchor.y));
  to_ax_ = to.screen_x(static_cast<float>(anchor.x));
  to_ay_ = to.screen_y(static_cast<float>(anchor.y));
  elapsed_ms_ = 0;
  active_ = true;
}

void ZoomTransition::advance(uint32_t dt_ms) noexcept {
  if (!active_) {
    return;
  }
  elapsed_ms_ += dt_ms;
  if (elapsed_ms_ >= duration_ms) {
    active_ = false;
  }
}

float ZoomTransition::eased() const noexcept {
  if (!active_) {
    return 1.0f;
  }
  return tween::ease_out_cubic(static_cast<float>(elapsed_ms_) /
                               static_cast<float>(duration_ms));
}

float ZoomTransition::zoom_at(float e) const noexcept {
  if (e <= 0.0f) {
    return static_cast<float>(from_.zoom);
  }
  if (e >= 1.0f) {
    return static_cast<float>(to_.zoom);
  }
  const float a = std::log(static_cast<float>(from_.zoom));
  const float b = std::log(static_cast<float>(to_.zoom));
  return std::exp(a + ((b - a) * e));
}

// world = anchor + (screen - anchor's screen position) * scale, with the
// anchor's screen position and the scale going from the old frame's to the
// new frame's. At e = 0 and e = 1 this is exactly that frame's mapping.
float ZoomTransition::world_x(float px, float e) const noexcept {
  const float ax = from_ax_ + ((to_ax_ - from_ax_) * std::clamp(e, 0.0f, 1.0f));
  return static_cast<float>(anchor_.x) + ((px - ax) * zoom_at(e));
}

float ZoomTransition::world_y(float py, float e) const noexcept {
  const float ay = from_ay_ + ((to_ay_ - from_ay_) * std::clamp(e, 0.0f, 1.0f));
  return static_cast<float>(anchor_.y) + ((py - ay) * zoom_at(e));
}

bool ZoomTransition::shows_new(ViewPoint p, float e) noexcept {
  // 4x4 ordered dither: at e the share e of the pixels shows the new frame.
  static constexpr std::array<int, 16> bayer = {0, 8,  2, 10, 12, 4, 14, 6,
                                                3, 11, 1, 9,  15, 7, 13, 5};
  const int b = bayer[static_cast<std::size_t>(((p.y & 3) * 4) + (p.x & 3))];
  return (static_cast<float>(b) + 0.5f) / 16.0f < e;
}

void ZoomTransition::composite(gfx::Surface& surface) const {
  if (!active_) {
    return;
  }
  const ViewRect area{std::max(to_.viewport.x, 0), std::max(to_.viewport.y, 0),
                      0, 0};
  const int x1 = std::min(to_.viewport.x + to_.viewport.w, surface.width());
  const int y1 = std::min(to_.viewport.y + to_.viewport.h, surface.height());
  if (x1 <= area.x || y1 <= area.y) {
    return;
  }
  const int w = x1 - area.x;
  const int h = y1 - area.y;

  // The new frame as drawn, before it is drawn over.
  const int pitch = surface.width();
  std::vector<uint8_t> new_pixels(static_cast<std::size_t>(w) *
                                  static_cast<std::size_t>(h));
  {
    const uint8_t* src = surface.data();
    for (int y = 0; y < h; ++y) {
      std::copy_n(src + (static_cast<std::size_t>(area.y + y) * pitch) + area.x,
                  w, new_pixels.begin() + (static_cast<std::ptrdiff_t>(y) * w));
    }
  }

  const float e = eased();
  const auto sample = [](const MapFrame& f, const uint8_t* pixels, int stride,
                         float wx, float wy, uint8_t& out) {
    const float sx = std::floor(f.screen_x(wx));
    const float sy = std::floor(f.screen_y(wy));
    const ViewPoint q{static_cast<int>(sx), static_cast<int>(sy)};
    if (!f.contains(q)) {
      return false;
    }
    out = pixels[(static_cast<std::size_t>(q.y - f.viewport.y) * stride) +
                 static_cast<std::size_t>(q.x - f.viewport.x)];
    return true;
  };
  // The new frame's pixels, addressed through its own mapping.
  MapFrame drawn_to = to_;
  drawn_to.viewport.x = area.x;
  drawn_to.viewport.y = area.y;
  drawn_to.viewport.w = w;
  drawn_to.viewport.h = h;
  drawn_to.top_left =
      WorldPoint{to_.top_left.x + ((area.x - to_.viewport.x) * to_.zoom),
                 to_.top_left.y + ((area.y - to_.viewport.y) * to_.zoom)};

  std::vector<uint8_t> out(new_pixels.size());
  for (int y = 0; y < h; ++y) {
    // The centre of the pixel, so both mappings agree at e = 0 and 1.
    const float wy = world_y(static_cast<float>(area.y + y) + 0.5f, e);
    for (int x = 0; x < w; ++x) {
      const ViewPoint p{area.x + x, area.y + y};
      const float wx = world_x(static_cast<float>(p.x) + 0.5f, e);
      uint8_t old_px = 0;
      uint8_t new_px = 0;
      const bool has_old =
          sample(from_, from_pixels_.data(), from_.viewport.w, wx, wy, old_px);
      const bool has_new =
          sample(drawn_to, new_pixels.data(), w, wx, wy, new_px);
      uint8_t px = new_pixels[(static_cast<std::size_t>(y) * w) + x];
      if (has_old && has_new) {
        px = shows_new(p, e) ? new_px : old_px;
      }
      else if (has_new) {
        px = new_px;
      }
      else if (has_old) {
        px = old_px;
      }
      out[(static_cast<std::size_t>(y) * w) + x] = px;
    }
  }

  // A plain fill hides the HD texels of the area, so the indexed pixels
  // written next show.
  surface.fill_rect(ViewRect{area.x, area.y, w, h}, 0);
  uint8_t* dst = surface.data();
  for (int y = 0; y < h; ++y) {
    std::copy_n(out.begin() + (static_cast<std::ptrdiff_t>(y) * w), w,
                dst + (static_cast<std::size_t>(area.y + y) * pitch) + area.x);
  }
}

} // namespace views::helpers
