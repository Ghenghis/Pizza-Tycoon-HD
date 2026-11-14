/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "engine/gfx/HdImage.h"

namespace gfx {

// One HD picture (or part of one) drawn onto a surface. Positions are in
// texels of the surface (engine pixel * hd_scale()).
struct HdCmd {
  std::shared_ptr<const HdImage> img;
  int sx{0}, sy{0}, sw{0}, sh{0}; // source rect, texels of img
  int dx{0}, dy{0};               // destination, texels of the surface
  bool half{false};               // drawn at half size (2x2 texels -> 1)
  // 0: the picture's own colours. Otherwise 0xRRGGBBFF: the picture is a
  // coverage mask (its alpha), drawn in this colour (HD text).
  uint32_t tint{0};

  [[nodiscard]] int dw() const noexcept { return half ? sw / 2 : sw; }
  [[nodiscard]] int dh() const noexcept { return half ? sh / 2 : sh; }
};

// The HD texels of a Surface, kept as the list of pictures drawn on it
// instead of a texel copy. The GPU draws the list directly, so drawing a
// sprite costs one textured quad instead of copying hd_scale()^2 texels per
// pixel on the CPU.
//
// The result is exactly the old texel-copy rules (see Surface.cpp):
//  * an opaque source pixel replaces the HD texels under it,
//  * a transparent source pixel blends its HD texels over them,
//  * a plain (non-HD) write clears them, so the indexed pixel shows.
// Per engine pixel, floor_of() is the first command that still shows there;
// every earlier command was replaced or cleared. needs_base() says whether
// the indexed pixel has to be redrawn just before that command: false when
// the command covers its block with opaque texels anyway.
class HdList {
public:
  // A single picture covering a whole width x height (engine pixel) surface.
  static std::shared_ptr<HdList> leaf(std::shared_ptr<const HdImage> img,
                                      int width, int height);

  HdList(int width, int height);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] const std::vector<HdCmd>& cmds() const noexcept {
    return cmds_;
  }
  [[nodiscard]] bool empty() const noexcept { return cmds_.empty(); }
  // Just one picture covering the whole surface, untouched since.
  [[nodiscard]] bool is_leaf() const noexcept { return leaf_; }

  [[nodiscard]] uint32_t floor_of(int x, int y) const noexcept;
  // Per engine pixel: floor | base_bit, or null when every floor is 0 (then
  // needs_base() comes from leaf_opacity()).
  [[nodiscard]] const uint32_t* floor_data() const noexcept {
    return floor_.empty() ? nullptr : floor_.data();
  }
  // For a leaf: one byte per engine pixel, 1 when its block is opaque.
  [[nodiscard]] const uint8_t* leaf_opacity() const;
  // Writable floors of one row (allocated on first use).
  [[nodiscard]] uint32_t* floor_row(int y);
  [[nodiscard]] bool needs_base(int x, int y) const noexcept;

  /* changes (the caller owns this list exclusively) */

  // A plain pixel landed on (x, y): every command so far is hidden there.
  void plain(int x, int y);
  void plain_rect(int x0, int y0, int x1, int y1);
  // Command `index` replaced the HD texels of (x, y).
  void replaced_by(int x, int y, uint32_t index, bool needs_base);
  // Appends and returns the new command's index.
  uint32_t push(HdCmd cmd);

  // Drops commands that are hidden everywhere, once enough have piled up
  // (only surfaces that are drawn on without being cleared get there).
  void compact_if_needed();

  // The texels this list stands for, as the old texel-copy code would have
  // left them. k is hd_scale().
  [[nodiscard]] HdImage materialize(int k) const;
  // The same for one rect of engine pixels only.
  [[nodiscard]] HdImage materialize(int k, int x, int y, int w, int h) const;

  static constexpr uint32_t base_bit = 0x80000000u;

private:
  void ensure_floor();

  int width_;
  int height_;
  bool leaf_{false};
  std::vector<HdCmd> cmds_;
  // Per engine pixel: floor index | base_bit. Empty means all zero, with
  // needs_base() derived from the leaf picture.
  std::vector<uint32_t> floor_;
  std::size_t compacted_size_{0};
};

// True when every texel of the k x k block at engine pixel (bx, by) of img is
// fully opaque. Cached per image.
[[nodiscard]] bool opaque_block(const HdImage& img, int bx, int by, int k);

// Value of one destination texel of a command (sampling rules included).
[[nodiscard]] uint32_t sample(const HdCmd& cmd, int tx, int ty);

// A texel of the command's picture in the command's colour (see
// HdCmd::tint).
[[nodiscard]] inline uint32_t tinted(const HdCmd& cmd, uint32_t v) noexcept {
  if (cmd.tint == 0) {
    return v;
  }
  const uint32_t a = v & 0xFFu;
  return a == 0 ? 0u : (cmd.tint & 0xFFFFFF00u) | a;
}

} // namespace gfx
