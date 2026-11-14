/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>
#include "engine/geom/Spaces.h"
#include "engine/gfx/Color.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Surface.h"
#include "views/hd/BackgroundJob.h"

// Water at the city view's CLOSE zoom in HD (hd-audit M4, the water part).
//
// The classic game animates water by cycling palette entries (140-143,
// a new step every 10 ticks); HD pack art has fixed colours, so in HD the
// water stood still. Here every cycle step that changes a colour gets one
// prebuilt picture: for each landsym tile type that has water, the tile's HD
// texels on its water pixels, recoloured to that step's colours, and
// transparent everywhere else. At CLOSE zoom the view draws, over each water
// tile it blitted, that tile from the current step's picture (one quad,
// hd_overlay, no floor changes). Nothing is repainted per frame, and the
// pictures go to the GPU once. Step 0 is the art as it is, so it needs no
// picture and nothing is drawn.
//
// Recolouring, per texel of a water pixel whose entry shows `from` at step 0
// and `to` at this step, per channel:
//   out = to + (texel - from) * clamp(to / from, 0.5, 2)
// so a texel in the palette colour becomes exactly the new colour, and the
// art's detail around it is kept (scaled with the colour, but never blown
// up on a dark channel). Alpha is the art's.
//
// Art only (no tint, no atlas): the 1994 look hides it and shows the
// indexed water, which cycles by palette as before. Nothing at hd_scale 1.
namespace views::hd {

class WaterFrames;

struct WaterCycleInput {
  std::vector<uint8_t> pixels; // landsym's indexed pixels
  int width = 0;               // landsym's size, engine pixels
  int height = 0;
  std::shared_ptr<const gfx::HdImage> hd; // landsym's HD picture, read-only
  // The palette the map is shown in at step 0 (season included).
  std::vector<gfx::Color> colors;
  // The cycled entries, and per step the entry each one shows:
  // colors[cycles[s][n]] is shown for indices[n] at step s.
  std::vector<uint8_t> indices;
  std::vector<std::vector<uint8_t>> cycles;
  int k = 1;
};

// The input from the landsym surface, or nothing at hd_scale 1 or without
// an HD landsym (no pack).
[[nodiscard]] std::optional<WaterCycleInput>
water_cycle_input(const gfx::Surface& landsym,
                  const std::vector<gfx::Color>& colors,
                  const std::vector<uint8_t>& indices,
                  const std::vector<std::vector<uint8_t>>& cycles);

// Builds the pictures (cached in the map picture cache, one per step). Null
// when stopped, when nothing has water, or when the input does not fit.
[[nodiscard]] std::shared_ptr<const WaterFrames>
build_water_frames(const WaterCycleInput& input, std::stop_token stop = {});

class WaterFrames {
public:
  // Engine pixels per CLOSE tile side (32).
  static int tile_px();

  // True when step `step` shows other colours than the art, i.e. there is
  // something to draw.
  [[nodiscard]] bool has_step(int step) const;
  // True when tiles of this landsym type have water.
  [[nodiscard]] bool has_water(uint8_t type) const { return slot_[type] >= 0; }
  // Draws the water of tile `type` at step `step` over the tile at `dest`
  // (engine pixels of `dst`), clipped to that tile. Call after the tile's
  // blit. False when there is nothing to draw (or hd_scale changed).
  bool overlay(gfx::Surface& dst, uint8_t type, ViewPoint dest, int step) const;

  [[nodiscard]] int k() const { return k_; }
  [[nodiscard]] int water_tiles() const { return slots_; }
  // The pictures, per step (null where the step needs none).
  [[nodiscard]] const std::vector<std::shared_ptr<const gfx::HdImage>>&
  pictures() const {
    return steps_;
  }
  [[nodiscard]] std::size_t bytes() const;

private:
  friend std::shared_ptr<const WaterFrames>
  build_water_frames(const WaterCycleInput& input, std::stop_token stop);
  WaterFrames() = default;

  int k_ = 1;
  int slots_ = 0;
  int columns_ = 1; // slots per row of a picture
  std::array<int16_t, 256> slot_{};
  std::vector<std::shared_ptr<const gfx::HdImage>> steps_;
};

using WaterJob = BackgroundJob<const WaterFrames>;

// Starts build_water_frames() on `job`'s worker; take the frames with
// job.take() on a later frame.
void start_water_frames(WaterJob& job, WaterCycleInput input);

} // namespace views::hd
