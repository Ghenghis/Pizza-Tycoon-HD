/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/util/Tween.h"
#include "engine/geom/Camera.h"
#include "engine/geom/Spaces.h"

// Where to draw a scrolling camera (smooth motion). The camera itself moves
// as it always did, in steps per simulation tick or at once on a drag, and
// stays what input, hit-testing and the saved map position use; this only
// says where to draw the picture:
//
//  * between ticks, alpha of the way from the scroll of the tick before to
//    the scroll of the last tick;
//  * while a drag moves the camera and for a moment after it is let go, a
//    critically damped spring following the camera, so the picture glides
//    into place instead of stopping dead.
//
// A change of zoom or view size, or a jump further than any scroll step
// (half a view), is drawn at once.
class CameraSmoother {
public:
  // How long the spring takes to close most of the gap.
  static constexpr float spring_smooth_time_ms = 45.0f;

  // Once per simulation tick, after the camera has moved for that tick.
  void on_tick(const Camera& camera) noexcept {
    if (!started_ || changed(camera)) {
      snap(camera);
      return;
    }
    prev_ = curr_;
    curr_ = camera.scroll();
    if (tween::is_teleport(prev_, curr_, max_step(camera))) {
      prev_ = curr_;
      if (springing_) {
        x_.snap(static_cast<float>(curr_.x));
        y_.snap(static_cast<float>(curr_.y));
      }
    }
  }

  // Once per frame with the frame's time. dragging: the camera is being
  // dragged right now.
  void advance(const Camera& camera, uint32_t dt_ms, float alpha,
               bool dragging) noexcept {
    if (!started_ || changed(camera)) {
      snap(camera);
      return;
    }
    const WorldPoint target = spring_target(camera, alpha, dragging);
    if (dragging && !springing_) {
      // Start from where the picture is now.
      const WorldPoint from = tween::lerp(prev_, curr_, alpha);
      x_.snap(static_cast<float>(from.x));
      y_.snap(static_cast<float>(from.y));
      springing_ = true;
    }
    if (!springing_) {
      return;
    }
    if (tween::is_teleport(spring_position(), target, max_step(camera))) {
      x_.snap(static_cast<float>(target.x));
      y_.snap(static_cast<float>(target.y));
    }
    const auto dt = static_cast<float>(dt_ms);
    x_.follow(static_cast<float>(target.x), dt, spring_smooth_time_ms);
    y_.follow(static_cast<float>(target.y), dt, spring_smooth_time_ms);
    if (!dragging && x_.settled(static_cast<float>(target.x)) &&
        y_.settled(static_cast<float>(target.y))) {
      springing_ = false;
    }
  }

  // Where to draw the camera this frame: the camera itself until the
  // smoother has seen it (a fresh view, a screenshot) or after it changed.
  [[nodiscard]] WorldPoint drawn(const Camera& camera,
                                 float alpha) const noexcept {
    if (!started_ || changed(camera)) {
      return camera.scroll();
    }
    if (springing_) {
      return spring_position();
    }
    return tween::drawn(prev_, curr_, alpha, max_step_);
  }

  // Whether the drawn camera can still differ from the camera, so the view
  // has to be drawn every frame.
  [[nodiscard]] bool moving() const noexcept {
    return springing_ || prev_ != curr_;
  }

  // Draws exactly the camera from now on (zoom change, map swap).
  void snap(const Camera& camera) noexcept {
    started_ = true;
    zoom_ = camera.zoom();
    view_size_ = camera.view_size();
    max_step_ = max_step(camera);
    prev_ = curr_ = camera.scroll();
    x_.snap(static_cast<float>(curr_.x));
    y_.snap(static_cast<float>(curr_.y));
    springing_ = false;
  }

  // Half a view in world pixels: more than any scroll moves in one tick.
  [[nodiscard]] static int max_step(const Camera& camera) noexcept {
    const Size v = camera.view_size();
    return (std::max(v.w, v.h) * std::max(camera.zoom(), 1)) / 2;
  }

private:
  [[nodiscard]] bool changed(const Camera& camera) const noexcept {
    return camera.zoom() != zoom_ || camera.view_size().w != view_size_.w ||
           camera.view_size().h != view_size_.h;
  }
  [[nodiscard]] WorldPoint spring_target(const Camera& camera, float alpha,
                                         bool dragging) const noexcept {
    // A drag moves the camera between ticks: follow it as it is. Otherwise
    // (the settle after a drag, the tile alignment) follow the tick motion.
    return dragging ? camera.scroll()
                    : tween::drawn(prev_, curr_, alpha, max_step_);
  }
  [[nodiscard]] WorldPoint spring_position() const noexcept {
    return WorldPoint{static_cast<int>(std::lround(x_.position)),
                      static_cast<int>(std::lround(y_.position))};
  }

  bool started_{false};
  int zoom_{1};
  Size view_size_{0, 0};
  int max_step_{0};
  WorldPoint prev_;
  WorldPoint curr_;
  bool springing_{false};
  tween::Spring x_;
  tween::Spring y_;
};
