/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <vector>

#include "engine/geom/Spaces.h"

namespace gfx {
class Surface;
}

namespace views::helpers {

// How one drawn frame of a map shows the world: the screen rect the map
// fills, the world pixel at its top left and how many world pixels one screen
// pixel covers.
struct MapFrame {
  ViewRect viewport;
  WorldPoint top_left;
  int zoom{1};

  // The screen pixel showing world point w, in floating point (it can fall
  // inside a pixel), and the world point a screen pixel shows.
  [[nodiscard]] float screen_x(float wx) const noexcept;
  [[nodiscard]] float screen_y(float wy) const noexcept;
  [[nodiscard]] bool contains(ViewPoint p) const noexcept;
};

// An eased change between two map scales (smooth motion), drawing only. The
// zoom itself happens at once, as before: input and hit-testing use the new
// scale from the first frame. For a short while the picture moves from the
// old scale to the new one around the point that was zoomed on: both frames
// are drawn through one camera whose scale goes from the old to the new
// (evenly in log scale, eased out), and an ordered dissolve hands over from
// the old frame to the new one, so palette pictures need no blending. At the
// end the new frame shows exactly as it would without the transition.
//
// The pictures are the indexed ones: while the transition runs, the HD
// texels of the map are hidden, as for any plain drawing.
class ZoomTransition {
public:
  static constexpr uint32_t duration_ms = 180;

  // from: the last frame drawn before the zoom and its pixels (row-major,
  // from.viewport's size); to: how the new frame shows the world; anchor:
  // the world point that was zoomed on.
  void begin(const MapFrame& from, std::vector<uint8_t> from_pixels,
             const MapFrame& to, WorldPoint anchor);
  void cancel() noexcept { active_ = false; }
  void advance(uint32_t dt_ms) noexcept;
  [[nodiscard]] bool active() const noexcept { return active_; }
  // 0..1 along the transition, eased.
  [[nodiscard]] float eased() const noexcept;

  // The world point that screen pixel p shows at eased progress e: the old
  // frame's at 0, the new frame's at 1.
  [[nodiscard]] float world_x(float px, float e) const noexcept;
  [[nodiscard]] float world_y(float py, float e) const noexcept;
  // Whether screen pixel p shows the new frame (rather than the old) at
  // eased progress e, where both have a picture there.
  [[nodiscard]] static bool shows_new(ViewPoint p, float e) noexcept;

  // Draws the transition over the new frame, which is already in surface
  // within the new frame's viewport.
  void composite(gfx::Surface& surface) const;

  [[nodiscard]] const MapFrame& from() const noexcept { return from_; }
  [[nodiscard]] const MapFrame& to() const noexcept { return to_; }

private:
  [[nodiscard]] float zoom_at(float e) const noexcept;

  bool active_{false};
  uint32_t elapsed_ms_{0};
  MapFrame from_;
  MapFrame to_;
  std::vector<uint8_t> from_pixels_;
  WorldPoint anchor_;
  // Where the anchor is on screen in each frame.
  float from_ax_{0.0f};
  float from_ay_{0.0f};
  float to_ax_{0.0f};
  float to_ay_{0.0f};
};

} // namespace views::helpers
