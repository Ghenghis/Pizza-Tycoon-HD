/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "engine/Size.h"
#include "engine/geom/Spaces.h"
#include "engine/gfx/ColorIndex.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/PaletteBank.h"
#include "engine/gfx/Upscaler.h"

namespace gfx {

constexpr uint8_t TRANSPARENT = 0;

// Original assets are scaled up by this on load.
constexpr int asset_pixel_scale = 2;

class Surface {
public:
  Surface(Size size, std::optional<PaletteHandle> palette = std::nullopt,
          ColorIndex fill_index = 0);
  Surface(int width, int height,
          std::optional<PaletteHandle> palette = std::nullopt,
          ColorIndex fill_index = 0);

  Surface(Surface&&) noexcept = default;
  Surface& operator=(Surface&&) noexcept = default;
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  [[nodiscard]] Surface clone() const;

  [[nodiscard]] static Surface
  from_indexed(std::span<const uint8_t> src, int width, int height,
               PaletteHandle palette, int scale = asset_pixel_scale,
               Upscaler upscaler = Upscaler::Double);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] PaletteHandle palette() const noexcept {
    return palette_.value_or(PaletteHandle{0});
  }
  void palette(PaletteHandle h) noexcept;

  /* pixel ops (index space) */
  void put(ViewPoint p, ColorIndex idx,
           std::optional<PaletteHandle> pal = std::nullopt);
  void fill_rect(ViewRect r, ColorIndex idx,
                 std::optional<PaletteHandle> pal = std::nullopt);
  void fill(ColorIndex idx, std::optional<PaletteHandle> pal = std::nullopt);
  [[nodiscard]] uint8_t get(ViewPoint p) const;

  // Full-featured: source rect + clipping + transparency
  void blit(const Surface& src, ViewRect src_rect,
            ViewPoint dst = ViewPoint{0, 0}, uint8_t transparent = TRANSPARENT);

  // Draws src_rect as a single-color shape: every non-transparent source
  // pixel becomes `color`, whatever index it was.
  void blit_stencil(const Surface& src, ViewRect src_rect, ViewPoint dst,
                    uint8_t color, uint8_t transparent = TRANSPARENT);

  // Fast path: full-width copy, no transparency, no clipping
  void blit_raw(const Surface& src, ViewPoint dst = ViewPoint{0, 0});

  // Transparency only
  void blit_masked(const Surface& src, ViewPoint dst = ViewPoint{0, 0},
                   uint8_t transparent = TRANSPARENT);

  /* raw access */
  [[nodiscard]] uint8_t* data() noexcept { return pixels_.data(); }
  [[nodiscard]] const uint8_t* data() const noexcept { return pixels_.data(); }
  [[nodiscard]] std::span<const uint8_t> pixels() const noexcept;

  // Replace every pixel whose index is in `keys` with `replacement`
  void replace_colors(std::initializer_list<uint8_t> keys, uint8_t replacement,
                      std::optional<ViewRect> region = std::nullopt) noexcept;

  void colors(const std::vector<Color>&);
  [[nodiscard]] std::vector<Color>& colors();
  [[nodiscard]] const std::vector<Color>& colors() const;
  void clear_custom_palette();
  void clear_palette_state();

  // Disable palette translation for this surface (useful for font atlases,
  // templates)
  void disable_palette_translation() { disable_palette_translation_ = true; }
  [[nodiscard]] bool palette_translation_disabled() const {
    return disable_palette_translation_;
  }

  /* HD overlay (see HdImage.h) */

  // Attach high-resolution pixels for this whole surface. The image must be
  // exactly width() * hd_scale() by height() * hd_scale(); it is shared, not
  // copied, until this surface is drawn on.
  void attach_hd(std::shared_ptr<const HdImage> hd);
  // The HD texels as one image, or null. Built from the draw list on first
  // use after a change (the renderer draws the list directly instead).
  [[nodiscard]] const HdImage* hd() const;
  [[nodiscard]] bool has_hd() const noexcept { return hd_ && !hd_->empty(); }
  // The HD pictures drawn on this surface, in order, or null.
  [[nodiscard]] const HdList* hd_list() const noexcept {
    return has_hd() ? hd_.get() : nullptr;
  }

  // Copies src_rect at half size, keeping one pixel of each 2x2 block, and
  // averaging the HD texels of the same block when there are any.
  void blit_half(const Surface& src, ViewRect src_rect, ViewPoint dst);

  [[nodiscard]] Size size() const { return Size{width_, height_}; }
  [[nodiscard]] ViewRect rect() const {
    return ViewRect{0, 0, width_, height_};
  }

private:
  // pitch is number of pixels per row in the surface
  int width_{0}, height_{0}, pitch_{0}; // pitch in *pixels*
  std::optional<PaletteHandle>
      palette_; // nullopt = uninitialized, first blit sets it
  std::optional<std::vector<Color>> custom_palette_;

  std::vector<uint8_t> pixels_; // size = width*height

  // Skip palette mapping (for fonts)
  bool disable_palette_translation_{false};

  // HD pictures drawn on this surface (see HdList.h), or null. Shared
  // copy-on-write: hd_mut() clones before the first change if anyone else
  // holds the list.
  std::shared_ptr<HdList> hd_;
  // hd() result for the current list.
  mutable std::shared_ptr<const HdImage> hd_image_;

  [[nodiscard]] HdList& hd_mut();
  // Hides the HD texels behind one engine pixel, so the indexed pixel shows.
  void hd_clear_pixel(int x, int y);
  void hd_clear_rect(int x0, int y0, int x1, int y1);
  // HD side of a blit from an HD source: the rect s of src lands at d.
  // `copy_all` copies even transparent source pixels (blit_raw).
  void hd_transfer(const Surface& src, ViewRect s, ViewPoint d, bool copy_all,
                   uint8_t transparent);

  [[nodiscard]] bool in_bounds(ViewPoint p) const noexcept;
  // Clips src_rect against both surfaces, moving dst to match. False when
  // nothing is left to draw.
  bool clip_blit(const Surface& src, ViewRect& src_rect, ViewPoint& dst) const;
};
} // namespace gfx
