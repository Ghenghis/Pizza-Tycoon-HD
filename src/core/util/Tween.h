/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Display-only motion maths (smooth motion): drawing a moving thing between
// the positions of its last two simulation steps. Nothing here is ever read
// back by the game: a drawn position is used for drawing only, never for game
// state, hit-testing or saves.
//
// Header-only, so the actors in core (city cars) and the views share it.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "engine/geom/Spaces.h"

namespace tween {

// a at t = 0, b at t = 1 exactly, rounded to the nearest pixel in between;
// t is clamped to 0..1.
[[nodiscard]] inline int lerp(int a, int b, float t) noexcept {
  if (t <= 0.0f) {
    return a;
  }
  if (t >= 1.0f) {
    return b;
  }
  return a + static_cast<int>(std::lround(static_cast<float>(b - a) * t));
}

template <typename Tag>
[[nodiscard]] inline TypedPoint<Tag> lerp(TypedPoint<Tag> a, TypedPoint<Tag> b,
                                          float t) noexcept {
  return TypedPoint<Tag>{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}

// A move from a to b further than max_step pixels along either axis: more
// than one step can move, so it is a teleport (a wrap, a jump, a respawn) and
// is drawn at once, never slid.
template <typename Tag>
[[nodiscard]] inline bool is_teleport(TypedPoint<Tag> a, TypedPoint<Tag> b,
                                      int max_step) noexcept {
  return std::abs(b.x - a.x) > max_step || std::abs(b.y - a.y) > max_step;
}

// Where to draw a thing that was at prev before its last step and is at curr
// now, alpha (0..1) of the way to its next step: prev at 0, curr at 1, and
// curr at once after a teleport.
template <typename Tag>
[[nodiscard]] inline TypedPoint<Tag> drawn(TypedPoint<Tag> prev,
                                           TypedPoint<Tag> curr, float alpha,
                                           int max_step) noexcept {
  if (is_teleport(prev, curr, max_step)) {
    return curr;
  }
  return lerp(prev, curr, alpha);
}

// 0 at 0, 1 at 1, fast at first and gently into the end.
[[nodiscard]] inline float ease_out_cubic(float t) noexcept {
  const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (u * u * u);
}

// One axis of a critically damped spring (no overshoot) following a moving
// target; smooth_time_ms is roughly the time to close most of the gap.
// Closed form of the damped step (Game Programming Gems 4, 1.10), stable for
// any frame time.
struct Spring {
  float position{0.0f};
  float velocity{0.0f}; // per millisecond

  void snap(float to) noexcept {
    position = to;
    velocity = 0.0f;
  }

  void follow(float target, float dt_ms, float smooth_time_ms) noexcept {
    if (dt_ms <= 0.0f) {
      return;
    }
    const float omega = 2.0f / std::max(smooth_time_ms, 1.0f);
    const float x = omega * dt_ms;
    const float decay =
        1.0f / (1.0f + x + (0.48f * x * x) + (0.235f * x * x * x));
    const float change = position - target;
    const float temp = (velocity + (omega * change)) * dt_ms;
    velocity = (velocity - (omega * temp)) * decay;
    position = target + ((change + temp) * decay);
  }

  // Within half a pixel of the target and hardly moving.
  [[nodiscard]] bool settled(float target) const noexcept {
    return std::abs(position - target) < 0.5f && std::abs(velocity) < 0.01f;
  }
};

} // namespace tween
