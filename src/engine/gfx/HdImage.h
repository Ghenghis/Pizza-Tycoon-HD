/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace gfx {

// High-resolution true-colour pixels that ride along with an indexed Surface.
//
// The engine draws in a 640x400 indexed space. When HD rendering is on, every
// engine pixel is backed by hd_scale() x hd_scale() texels of this image. A
// texel with alpha 0 means "no HD data here": the indexed pixel underneath is
// shown instead (upscaled). Texels are straight (non-premultiplied) alpha,
// packed the same way as SDL_PIXELFORMAT_RGBA8888: 0xRRGGBBAA.
struct HdImage {
  HdImage() = default;
  HdImage(int w, int h, uint32_t fill = 0);
  // Copies start without the opacity cache: the copy may be edited.
  HdImage(const HdImage& o)
      : width(o.width), height(o.height), pixels(o.pixels) {}
  HdImage& operator=(const HdImage& o) {
    width = o.width;
    height = o.height;
    pixels = o.pixels;
    opaque_blocks.reset();
    opaque_blocks_k = 0;
    return *this;
  }
  HdImage(HdImage&&) noexcept = default;
  HdImage& operator=(HdImage&&) noexcept = default;

  int width = 0;
  int height = 0;
  std::vector<uint32_t> pixels;

  [[nodiscard]] uint32_t at(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * width + x];
  }
  [[nodiscard]] uint32_t& at(int x, int y) {
    return pixels[static_cast<std::size_t>(y) * width + x];
  }
  [[nodiscard]] uint32_t* row(int y) {
    return pixels.data() + static_cast<std::size_t>(y) * width;
  }
  [[nodiscard]] const uint32_t* row(int y) const {
    return pixels.data() + static_cast<std::size_t>(y) * width;
  }

  // Cache for gfx::opaque_block(): one byte per k x k block, 1 when every
  // texel in it is fully opaque. Filled on first use; the pixels of an image
  // that is shared are never changed, so the cache stays valid.
  mutable std::shared_ptr<const std::vector<uint8_t>> opaque_blocks;
  mutable int opaque_blocks_k = 0;
};

inline constexpr int max_hd_scale = 8;

// HD texels per engine pixel. 1 means HD rendering is off. Set once at
// startup, before any asset loads.
void hd_scale(int scale);
[[nodiscard]] int hd_scale() noexcept;

[[nodiscard]] constexpr uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b,
                                           uint32_t a) noexcept {
  return (r << 24) | (g << 16) | (b << 8) | a;
}
[[nodiscard]] constexpr uint32_t red(uint32_t p) noexcept { return p >> 24; }
[[nodiscard]] constexpr uint32_t green(uint32_t p) noexcept {
  return (p >> 16) & 0xFF;
}
[[nodiscard]] constexpr uint32_t blue(uint32_t p) noexcept {
  return (p >> 8) & 0xFF;
}
[[nodiscard]] constexpr uint32_t alpha(uint32_t p) noexcept { return p & 0xFF; }

// Porter-Duff "source over destination" with straight alpha.
[[nodiscard]] uint32_t blend_over(uint32_t src, uint32_t dst) noexcept;

// Multiplies the colour channels by factor (0..1), keeping alpha. Used for
// screen fades.
[[nodiscard]] uint32_t scale_rgb(uint32_t p, float factor) noexcept;

// Resizes with alpha-correct filtering: an area average when shrinking and
// bilinear interpolation when growing, done per axis in premultiplied space
// so transparent texels never bleed their colour into the edges.
[[nodiscard]] HdImage resample(const HdImage& src, int width, int height);

} // namespace gfx
