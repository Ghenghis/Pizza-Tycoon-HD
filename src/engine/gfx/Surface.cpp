/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/gfx/Surface.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <numeric>
#include "engine/Fatal.h"
#include "engine/gfx/Mmpx.h"
#include "engine/gfx/PaletteBank.h"

namespace gfx {

Surface::Surface(Size size, std::optional<PaletteHandle> palette,
                 ColorIndex fill_index)
    : Surface(size.w, size.h, palette, fill_index) {}

Surface::Surface(int width, int height, std::optional<PaletteHandle> palette,
                 ColorIndex fill_index)
    : width_(width), height_(height), pitch_(width), palette_(palette) {

  if (width_ <= 0 || height_ <= 0) {
    fatal("Surface: dimensions must be positive");
  }

  pixels_.resize(width_ * height_, fill_index.get()); // index 0 = transparent
}

Surface Surface::from_indexed(std::span<const uint8_t> src, int w, int h,
                              PaletteHandle pal, int scale, Upscaler upscaler) {

  if (src.size() != static_cast<std::size_t>(w * h)) {
    fatal("Surface::from_indexed: size mismatch");
  }

  Surface dst(w * scale, h * scale, pal, /*scale=*/1);

  if (scale == 1) {
    std::memcpy(dst.pixels_.data(), src.data(), src.size());
    return dst;
  }

  if (upscaler == Upscaler::Mmpx) {
    mmpx(src.data(), w, w, h, dst.pixels_.data(), dst.pitch_,
         UpscaleEdge::Repeat, luma_table(pal));
    return dst;
  }

  // scale == 2 (simple pixel-doubling)
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t v = src[y * w + x];

      int dx = x * 2;
      int dy = y * 2;

      dst.pixels_[(dy)*dst.pitch_ + dx] = v;
      dst.pixels_[(dy)*dst.pitch_ + dx + 1] = v;
      dst.pixels_[(dy + 1) * dst.pitch_ + dx] = v;
      dst.pixels_[(dy + 1) * dst.pitch_ + dx + 1] = v;
    }
  }
  return dst;
}

Surface Surface::clone() const {
  Surface copy(width_, height_, palette_.value_or(PaletteHandle{0}));
  std::memcpy(copy.data(), pixels_.data(), pixels_.size());
  copy.hd_ = hd_; // shared until either side draws
  copy.hd_image_ = hd_image_;
  return copy;
}

/* HD overlay */

void Surface::attach_hd(std::shared_ptr<const HdImage> hd) {
  const int k = hd_scale();
  if (k <= 1) {
    fatal("Surface::attach_hd: HD rendering is off (hd_scale is 1)");
  }
  if (!hd || hd->width != width_ * k || hd->height != height_ * k) {
    fatal("Surface::attach_hd: HD image does not match surface size");
  }
  hd_image_ = hd;
  hd_ = HdList::leaf(std::move(hd), width_, height_);
}

const HdImage* Surface::hd() const {
  if (!has_hd()) {
    return nullptr;
  }
  if (!hd_image_) {
    if (hd_->is_leaf()) {
      hd_image_ = hd_->cmds().front().img;
    }
    else {
      hd_image_ = std::make_shared<const HdImage>(hd_->materialize(hd_scale()));
    }
  }
  return hd_image_.get();
}

HdList& Surface::hd_mut() {
  if (!hd_) {
    hd_ = std::make_shared<HdList>(width_, height_);
  }
  else if (hd_.use_count() > 1) {
    hd_ = std::make_shared<HdList>(*hd_);
  }
  hd_image_.reset();
  return *hd_;
}

void Surface::hd_clear_rect(int x0, int y0, int x1, int y1) {
  if (!has_hd() || x1 <= x0 || y1 <= y0) {
    return;
  }
  hd_mut().plain_rect(x0, y0, x1, y1);
}

void Surface::hd_overlay(HdCmd cmd, int clip_x0, int clip_y0, int clip_x1,
                         int clip_y1) {
  const int k = hd_scale();
  const int x0 = std::max({cmd.dx, clip_x0, 0});
  const int y0 = std::max({cmd.dy, clip_y0, 0});
  const int x1 = std::min({cmd.dx + cmd.dw(), clip_x1, width_ * k});
  const int y1 = std::min({cmd.dy + cmd.dh(), clip_y1, height_ * k});
  if (x1 <= x0 || y1 <= y0 || !cmd.img) {
    return;
  }
  const int scale = cmd.half ? 2 : 1;
  cmd.sx += (x0 - cmd.dx) * scale;
  cmd.sy += (y0 - cmd.dy) * scale;
  cmd.sw = (x1 - x0) * scale;
  cmd.sh = (y1 - y0) * scale;
  cmd.dx = x0;
  cmd.dy = y0;
  // Blended over everything before it, like a blit whose pixels are all
  // transparent: no floor changes.
  HdList& list = hd_mut();
  list.push(std::move(cmd));
  list.compact_if_needed();
}

void Surface::hd_clear_pixel(int x, int y) {
  if (!has_hd()) {
    return;
  }
  hd_mut().plain(x, y);
}

void Surface::hd_transfer(const Surface& src, ViewRect s, ViewPoint d,
                          bool copy_all, uint8_t transparent) {
  const int k = hd_scale();
  // Keeps the source list alive and unchanged even when src is *this.
  std::shared_ptr<const HdList> keep = src.hd_;
  const HdList* from = keep.get();
  int fx0 = s.x; // the rect's origin within `from`
  int fy0 = s.y;

  // A transparent source pixel blends the source's HD texels over ours,
  // which is drawing the source's commands again, unless some of them were
  // hidden on that pixel. Then the rect is flattened to one picture first.
  if (!copy_all && !from->is_leaf()) {
    bool flatten = false;
    for (int row = 0; row < s.h && !flatten; ++row) {
      const uint8_t* src_row = &src.pixels_[(s.y + row) * src.pitch_ + s.x];
      for (int col = 0; col < s.w; ++col) {
        if (src_row[col] == transparent &&
            from->floor_of(s.x + col, s.y + row) != 0) {
          flatten = true;
          break;
        }
      }
    }
    if (flatten) {
      keep = HdList::leaf(std::make_shared<const HdImage>(
                              from->materialize(k, s.x, s.y, s.w, s.h)),
                          s.w, s.h);
      from = keep.get();
      fx0 = 0;
      fy0 = 0;
    }
  }

  HdList& to = hd_mut();
  const auto& cmds = from->cmds();
  const std::size_t n = cmds.size();
  const int cx0 = fx0 * k;
  const int cy0 = fy0 * k;
  const int cx1 = (fx0 + s.w) * k;
  const int cy1 = (fy0 + s.h) * k;
  const int offx = d.x * k - cx0;
  const int offy = d.y * k - cy0;
  std::vector<uint32_t> next_kept(n + 1);
  std::vector<int64_t> kept(n, -1);
  for (std::size_t i = 0; i < n; ++i) {
    const HdCmd& c = cmds[i];
    const int x0 = std::max(c.dx, cx0);
    const int y0 = std::max(c.dy, cy0);
    const int x1 = std::min(c.dx + c.dw(), cx1);
    const int y1 = std::min(c.dy + c.dh(), cy1);
    if (x1 <= x0 || y1 <= y0) {
      continue;
    }
    HdCmd m = c;
    const int scale = c.half ? 2 : 1;
    m.sx = c.sx + (x0 - c.dx) * scale;
    m.sy = c.sy + (y0 - c.dy) * scale;
    m.sw = (x1 - x0) * scale;
    m.sh = (y1 - y0) * scale;
    m.dx = x0 + offx;
    m.dy = y0 + offy;
    kept[i] = to.push(std::move(m));
  }
  const auto end = static_cast<uint32_t>(to.cmds().size());
  next_kept[n] = end;
  for (std::size_t i = n; i-- > 0;) {
    next_kept[i] =
        kept[i] >= 0 ? static_cast<uint32_t>(kept[i]) : next_kept[i + 1];
  }
  // Per pixel: where the source pixel is copied, our floor becomes the
  // source's (renumbered), so exactly the same commands show through.
  const uint32_t* from_floor = from->floor_data();
  const uint8_t* from_opaque = from->leaf_opacity(); // leaf: floor all 0
  const int from_w = from->width();
  for (int row = 0; row < s.h; ++row) {
    const uint8_t* src_row = &src.pixels_[(s.y + row) * src.pitch_ + s.x];
    uint32_t* dst_floor = to.floor_row(d.y + row) + d.x;
    const std::size_t from_off =
        static_cast<std::size_t>(fy0 + row) * from_w + fx0;
    for (int col = 0; col < s.w; ++col) {
      if (!copy_all && src_row[col] == transparent) {
        continue; // blended over: the commands above draw it
      }
      uint32_t f = 0;
      bool base = true;
      if (from_floor) {
        const uint32_t v = from_floor[from_off + col];
        f = v & ~HdList::base_bit;
        base = (v & HdList::base_bit) != 0;
      }
      else if (from_opaque) {
        base = from_opaque[from_off + col] == 0;
      }
      const uint32_t first = next_kept[f];
      dst_floor[col] = first | ((base || first == end) ? HdList::base_bit : 0);
    }
  }
  to.compact_if_needed();
}

/* Check if the point is within this surface */
inline bool Surface::in_bounds(ViewPoint p) const noexcept {
  return p.x >= 0 && p.x < width_ && p.y >= 0 && p.y < height_;
}

void Surface::palette(PaletteHandle h) noexcept {
  palette_ = h;
  custom_palette_.reset();
}

/* put a pixel at a specific point */
void Surface::put(ViewPoint p, ColorIndex idx,
                  std::optional<PaletteHandle> pal) {
  (void)pal;
  if (!in_bounds(p)) {
    return;
  }
  pixels_[(p.y * pitch_) + p.x] = idx.get();
  hd_clear_pixel(p.x, p.y);
}

/* fill, all the pixels will be updated */
void Surface::fill(ColorIndex idx, std::optional<PaletteHandle> pal) {
  (void)pal;
  std::memset(pixels_.data(), idx.get(), height_ * pitch_);
  hd_.reset();
  hd_image_.reset();
}

/* fill_rect, give it a rect and all the pixels in it will be updated */
void Surface::fill_rect(ViewRect r, ColorIndex idx,
                        std::optional<PaletteHandle> pal) {
  (void)pal;
  if (r.w <= 0 || r.h <= 0) {
    return;
  }

  const int x0 = std::max(0, r.x);
  const int y0 = std::max(0, r.y);
  const int x1 = std::min(width_, r.x + r.w);
  const int y1 = std::min(height_, r.y + r.h);

  for (int y = y0; y < y1; ++y) {
    std::fill_n(&pixels_[y * pitch_ + x0], x1 - x0, idx.get());
  }
  hd_clear_rect(x0, y0, x1, y1);
}

// blit  (no transparency / colorkey yet: copy all bytes)
void Surface::blit_raw(const Surface& src, ViewPoint dst) {
  for (int sy = 0; sy < src.height_; ++sy) {
    const int dy = dst.y + sy;
    if (dy < 0 || dy >= height_) {
      continue;
    }

    int copy_w = src.width_;
    int sx = 0;
    int dx = dst.x;

    if (dx < 0) {
      sx -= dx;
      copy_w += dx;
      dx = 0;
    }
    if (dx + copy_w > width_) {
      copy_w = width_ - dx;
    }
    if (copy_w <= 0) {
      continue;
    }

    const uint8_t* src_row = &src.pixels_[(sy * src.pitch_) + sx];
    uint8_t* dst_row = &pixels_[(dy * pitch_) + dx];
    std::memcpy(dst_row, src_row, static_cast<std::size_t>(copy_w));
  }

  if (!src.has_hd() && !has_hd()) {
    return;
  }
  // The same clipping as above, for the whole block at once.
  const int sx0 = std::max(0, -dst.x);
  const int sy0 = std::max(0, -dst.y);
  const int sx1 = std::min(src.width_, width_ - dst.x);
  const int sy1 = std::min(src.height_, height_ - dst.y);
  if (sx1 <= sx0 || sy1 <= sy0) {
    return;
  }
  if (!src.has_hd()) {
    hd_clear_rect(dst.x + sx0, dst.y + sy0, dst.x + sx1, dst.y + sy1);
    return;
  }
  hd_transfer(src, ViewRect{sx0, sy0, sx1 - sx0, sy1 - sy0},
              ViewPoint{dst.x + sx0, dst.y + sy0}, true, 0);
}

/* Full blit with support for transparency, source and destination rects */
bool Surface::clip_blit(const Surface& src, ViewRect& s, ViewPoint& d) const {
  // Clip src_rect to src surface
  if (s.x < 0) {
    d.x -= s.x;
    s.w += s.x;
    s.x = 0;
  }
  if (s.y < 0) {
    d.y -= s.y;
    s.h += s.y;
    s.y = 0;
  }
  if (s.x + s.w > src.width_) {
    s.w = src.width_ - s.x;
  }
  if (s.y + s.h > src.height_) {
    s.h = src.height_ - s.y;
  }
  if (s.w <= 0 || s.h <= 0) {
    return false;
  }

  // Clip against *this* surface
  if (d.x < 0) {
    s.x -= d.x;
    s.w += d.x;
    d.x = 0;
  }
  if (d.y < 0) {
    s.y -= d.y;
    s.h += d.y;
    d.y = 0;
  }
  if (d.x + s.w > width_) {
    s.w = width_ - d.x;
  }
  if (d.y + s.h > height_) {
    s.h = height_ - d.y;
  }
  return s.w > 0 && s.h > 0;
}

void Surface::blit(const Surface& src, ViewRect s, ViewPoint d,
                   uint8_t transparent) {
  if (!clip_blit(src, s, d)) {
    return;
  }

  // Transfer line by line
  for (int row = 0; row < s.h; ++row) {
    const uint8_t* src_row = &src.pixels_[(s.y + row) * src.pitch_ + s.x];
    uint8_t* dst_row = &pixels_[(d.y + row) * pitch_ + d.x];

    for (int col = 0; col < s.w; ++col) {
      uint8_t pix = src_row[col];
      if (pix != transparent) {
        dst_row[col] = pix;
      }
    }
  }

  if (!src.has_hd()) {
    if (!has_hd()) {
      return;
    }
    // Plain pixels landed on an HD picture: let them show through.
    for (int row = 0; row < s.h; ++row) {
      const uint8_t* src_row = &src.pixels_[(s.y + row) * src.pitch_ + s.x];
      for (int col = 0; col < s.w; ++col) {
        if (src_row[col] != transparent) {
          hd_clear_pixel(d.x + col, d.y + row);
        }
      }
    }
    return;
  }

  // HD source. Where the source pixel is opaque, its HD block replaces what
  // was there (the indexed pixel underneath is now the source's own pixel).
  // Where it is transparent, the HD texels may still carry soft edges that
  // reach past the original outline, so they are blended over.
  hd_transfer(src, s, d, false, transparent);
}

void Surface::blit_stencil(const Surface& src, ViewRect s, ViewPoint d,
                           uint8_t color, uint8_t transparent) {
  if (!clip_blit(src, s, d)) {
    return;
  }

  for (int row = 0; row < s.h; ++row) {
    const uint8_t* src_row = &src.pixels_[(s.y + row) * src.pitch_ + s.x];
    uint8_t* dst_row = &pixels_[(d.y + row) * pitch_ + d.x];

    for (int col = 0; col < s.w; ++col) {
      if (src_row[col] != transparent) {
        dst_row[col] = color;
        hd_clear_pixel(d.x + col, d.y + row);
      }
    }
  }
}

void Surface::blit_half(const Surface& src, ViewRect src_rect, ViewPoint dst) {
  const int w = src_rect.w / 2;
  const int h = src_rect.h / 2;
  // Pixels where both ends are inside their surfaces form one rectangle.
  auto range = [](int n, int to0, int to_size, int from0, int from_size) {
    int lo = std::max({0, -to0, (-from0 + 1) / 2});
    int hi = std::min({n, to_size - to0, (from_size - from0 + 1) / 2});
    return std::pair{lo, std::max(lo, hi)};
  };
  const auto [xa, xb] = range(w, dst.x, width_, src_rect.x, src.width_);
  const auto [ya, yb] = range(h, dst.y, height_, src_rect.y, src.height_);
  if (xa >= xb || ya >= yb) {
    return;
  }
  for (int y = ya; y < yb; ++y) {
    for (int x = xa; x < xb; ++x) {
      const ViewPoint from{src_rect.x + (x * 2), src_rect.y + (y * 2)};
      pixels_[(dst.y + y) * pitch_ + dst.x + x] = src.get(from);
    }
  }
  if (!src.has_hd()) {
    hd_clear_rect(dst.x + xa, dst.y + ya, dst.x + xb, dst.y + yb);
    return;
  }
  // The 2x2 source pixel blocks, flattened to one picture (zero beyond the
  // source's edge), drawn at half size.
  const int k = hd_scale();
  const int fx = src_rect.x + xa * 2;
  const int fy = src_rect.y + ya * 2;
  const int fw = (xb - xa) * 2;
  const int fh = (yb - ya) * 2;
  const int cw = std::min(fw, src.width_ - fx);
  const int ch = std::min(fh, src.height_ - fy);
  auto img = std::make_shared<HdImage>(fw * k, fh * k, 0u);
  if (src.hd_->is_leaf()) {
    const HdImage& in = *src.hd_->cmds().front().img;
    for (int ty = 0; ty < ch * k; ++ty) {
      std::memcpy(img->row(ty), in.row(fy * k + ty) + fx * k,
                  static_cast<std::size_t>(cw) * k * sizeof(uint32_t));
    }
  }
  else {
    const HdImage part = src.hd_->materialize(k, fx, fy, cw, ch);
    for (int ty = 0; ty < part.height; ++ty) {
      std::memcpy(img->row(ty), part.row(ty),
                  static_cast<std::size_t>(part.width) * sizeof(uint32_t));
    }
  }
  HdCmd cmd;
  cmd.sw = img->width;
  cmd.sh = img->height;
  cmd.dx = (dst.x + xa) * k;
  cmd.dy = (dst.y + ya) * k;
  cmd.half = true;
  cmd.img = std::move(img);
  HdList& to = hd_mut();
  const uint32_t index = to.push(std::move(cmd));
  for (int y = ya; y < yb; ++y) {
    for (int x = xa; x < xb; ++x) {
      to.replaced_by(dst.x + x, dst.y + y, index, true);
    }
  }
  to.compact_if_needed();
}

/* Blit masked, so respect transparency */
void Surface::blit_masked(const Surface& src, ViewPoint dst,
                          uint8_t transparent) {
  blit(src, {0, 0, src.width_, src.height_}, dst, transparent);
}

uint8_t Surface::get(ViewPoint p) const {
  assert(in_bounds(p) && "Surface::get out-of-bounds");
  return pixels_[p.y * pitch_ + p.x];
}

void Surface::replace_colors(std::initializer_list<uint8_t> keys,
                             uint8_t replacement,
                             std::optional<ViewRect> region) noexcept {
  // Let's agree you don't ask to replace the existing color, but only
  // if it's the first color.
  if (*keys.begin() == replacement) {
    return;
  }
  std::array<uint8_t, 256> map;
  std::iota(map.begin(), map.end(), 0);
  for (uint8_t k : keys) {
    map[k] = replacement;
  }

  // Clamp region to the surface if needed, otherwise just do whole surface
  int x0 = 0;
  int y0 = 0;
  int x1 = width_;
  int y1 = height_;
  if (region) {
    x0 = std::clamp(region->x, 0, width_);
    y0 = std::clamp(region->y, 0, height_);
    x1 = std::clamp(region->x + region->w, 0, width_);
    y1 = std::clamp(region->y + region->h, 0, height_);
  }

  for (int y = y0; y < y1; ++y) {
    uint8_t* row = pixels_.data() + (y * pitch_) + x0;
    for (int x = x0; x < x1; ++x) {
      const uint8_t before = row[x - x0];
      row[x - x0] = map[before];
      if (row[x - x0] != before) {
        hd_clear_pixel(x, y);
      }
    }
  }
}

std::span<const uint8_t> Surface::pixels() const noexcept {
  return {pixels_.data(), pixels_.size()};
}

const std::vector<Color>& Surface::colors() const {
  if (!custom_palette_) {
    return PaletteBank::instance().colors(palette_.value_or(PaletteHandle{0}));
  }
  return *custom_palette_;
}

std::vector<Color>& Surface::colors() {
  if (!custom_palette_) {
    custom_palette_ =
        PaletteBank::instance().colors(palette_.value_or(PaletteHandle{0}));
  }
  return *custom_palette_;
}

void Surface::colors(const std::vector<Color>& custom_palette) {
  custom_palette_ = custom_palette;
}

void Surface::clear_custom_palette() { custom_palette_.reset(); }

void Surface::clear_palette_state() {
  palette_ = std::nullopt;
  custom_palette_.reset();
}

} // namespace gfx
