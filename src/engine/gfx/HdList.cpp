/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/gfx/HdList.h"

#include <algorithm>
#include <cassert>
#include "engine/Fatal.h"

namespace gfx {

namespace {

// Commands are compacted once there are this many and twice as many as
// after the last compaction.
constexpr std::size_t compact_threshold = 512;

uint32_t sample_half(const HdImage& in, int x0, int y0) {
  // Same arithmetic as the old Surface::blit_half: premultiplied average of
  // 2x2 texels, rounded.
  uint32_t r = 0;
  uint32_t g = 0;
  uint32_t b = 0;
  uint32_t a = 0;
  for (int sy = 0; sy < 2; ++sy) {
    for (int sx = 0; sx < 2; ++sx) {
      const uint32_t p = in.at(x0 + sx, y0 + sy);
      const uint32_t pa = alpha(p);
      r += red(p) * pa;
      g += green(p) * pa;
      b += blue(p) * pa;
      a += pa;
    }
  }
  return a == 0 ? 0u
                : pack_rgba((r + a / 2) / a, (g + a / 2) / a, (b + a / 2) / a,
                            (a + 2) / 4);
}

// Opacity of the k x k block whose top-left texel in the command's picture
// lands on engine pixel (px, py).
bool cmd_opaque_at(const HdCmd& c, int px, int py, int k) {
  if (c.half) {
    return false;
  }
  const int ix = c.sx + (px * k - c.dx);
  const int iy = c.sy + (py * k - c.dy);
  if (ix % k == 0 && iy % k == 0) {
    return opaque_block(*c.img, ix / k, iy / k, k);
  }
  for (int y = 0; y < k; ++y) {
    const uint32_t* row = c.img->row(iy + y) + ix;
    for (int x = 0; x < k; ++x) {
      if (alpha(row[x]) != 255) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

bool opaque_block(const HdImage& img, int bx, int by, int k) {
  if (!img.opaque_blocks || img.opaque_blocks_k != k) {
    const int bw = img.width / k;
    const int bh = img.height / k;
    auto blocks = std::make_shared<std::vector<uint8_t>>(
        static_cast<std::size_t>(bw) * bh, uint8_t{0});
    for (int y = 0; y < bh; ++y) {
      for (int x = 0; x < bw; ++x) {
        bool opaque = true;
        for (int ty = 0; ty < k && opaque; ++ty) {
          const uint32_t* row = img.row(y * k + ty) + x * k;
          for (int tx = 0; tx < k; ++tx) {
            if (alpha(row[tx]) != 255) {
              opaque = false;
              break;
            }
          }
        }
        (*blocks)[static_cast<std::size_t>(y) * bw + x] = opaque ? 1 : 0;
      }
    }
    img.opaque_blocks = std::move(blocks);
    img.opaque_blocks_k = k;
  }
  const int bw = img.width / k;
  if (bx < 0 || by < 0 || bx >= bw || by >= img.height / k) {
    return false;
  }
  return (*img.opaque_blocks)[static_cast<std::size_t>(by) * bw + bx] != 0;
}

uint32_t sample(const HdCmd& cmd, int tx, int ty) {
  if (cmd.half) {
    return sample_half(*cmd.img, cmd.sx + (tx - cmd.dx) * 2,
                       cmd.sy + (ty - cmd.dy) * 2);
  }
  return cmd.img->at(cmd.sx + (tx - cmd.dx), cmd.sy + (ty - cmd.dy));
}

std::shared_ptr<HdList> HdList::leaf(std::shared_ptr<const HdImage> img,
                                     int width, int height) {
  auto list = std::make_shared<HdList>(width, height);
  HdCmd cmd;
  cmd.sw = img->width;
  cmd.sh = img->height;
  cmd.img = std::move(img);
  list->cmds_.push_back(std::move(cmd));
  list->leaf_ = true;
  return list;
}

HdList::HdList(int width, int height) : width_(width), height_(height) {}

uint32_t HdList::floor_of(int x, int y) const noexcept {
  if (floor_.empty()) {
    return 0;
  }
  return floor_[static_cast<std::size_t>(y) * width_ + x] & ~base_bit;
}

bool HdList::needs_base(int x, int y) const noexcept {
  if (floor_.empty()) {
    if (leaf_) {
      return !cmd_opaque_at(cmds_.front(), x, y, hd_scale());
    }
    return true;
  }
  return (floor_[static_cast<std::size_t>(y) * width_ + x] & base_bit) != 0;
}

const uint8_t* HdList::leaf_opacity() const {
  if (!leaf_) {
    return nullptr;
  }
  const HdImage& img = *cmds_.front().img;
  const int k = hd_scale();
  (void)opaque_block(img, 0, 0, k); // fills the cache
  return img.opaque_blocks->data();
}

uint32_t* HdList::floor_row(int y) {
  ensure_floor();
  return floor_.data() + static_cast<std::size_t>(y) * width_;
}

void HdList::ensure_floor() {
  if (!floor_.empty()) {
    leaf_ = false;
    return;
  }
  floor_.assign(static_cast<std::size_t>(width_) * height_, base_bit);
  if (leaf_) {
    const int k = hd_scale();
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        if (cmd_opaque_at(cmds_.front(), x, y, k)) {
          floor_[static_cast<std::size_t>(y) * width_ + x] = 0;
        }
      }
    }
  }
  leaf_ = false;
}

void HdList::plain(int x, int y) {
  if (cmds_.empty()) {
    return;
  }
  ensure_floor();
  floor_[static_cast<std::size_t>(y) * width_ + x] =
      static_cast<uint32_t>(cmds_.size()) | base_bit;
}

void HdList::plain_rect(int x0, int y0, int x1, int y1) {
  if (cmds_.empty() || x1 <= x0 || y1 <= y0) {
    return;
  }
  ensure_floor();
  const uint32_t v = static_cast<uint32_t>(cmds_.size()) | base_bit;
  for (int y = y0; y < y1; ++y) {
    std::fill_n(floor_.begin() + static_cast<std::ptrdiff_t>(y) * width_ + x0,
                x1 - x0, v);
  }
}

void HdList::replaced_by(int x, int y, uint32_t index, bool base) {
  ensure_floor();
  floor_[static_cast<std::size_t>(y) * width_ + x] =
      index | (base ? base_bit : 0);
}

uint32_t HdList::push(HdCmd cmd) {
  ensure_floor();
  cmds_.push_back(std::move(cmd));
  return static_cast<uint32_t>(cmds_.size() - 1);
}

void HdList::compact_if_needed() {
  if (cmds_.size() < compact_threshold || cmds_.size() < 2 * compacted_size_) {
    return;
  }
  const int k = hd_scale();
  const std::size_t n = cmds_.size();
  std::vector<bool> visible(n, false);
  for (std::size_t i = 0; i < n; ++i) {
    const HdCmd& c = cmds_[i];
    const int x0 = std::max(0, c.dx / k);
    const int y0 = std::max(0, c.dy / k);
    const int x1 = std::min(width_, (c.dx + c.dw() + k - 1) / k);
    const int y1 = std::min(height_, (c.dy + c.dh() + k - 1) / k);
    for (int y = y0; y < y1 && !visible[i]; ++y) {
      for (int x = x0; x < x1; ++x) {
        if (floor_of(x, y) <= i) {
          visible[i] = true;
          break;
        }
      }
    }
  }
  // next_kept[f]: new index of the first kept command at or after f.
  std::vector<uint32_t> next_kept(n + 1);
  std::vector<HdCmd> kept;
  kept.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (visible[i]) {
      kept.push_back(std::move(cmds_[i]));
    }
  }
  uint32_t idx = static_cast<uint32_t>(kept.size());
  next_kept[n] = idx;
  for (std::size_t i = n; i-- > 0;) {
    if (visible[i]) {
      --idx;
    }
    next_kept[i] = idx;
  }
  for (uint32_t& f : floor_) {
    f = next_kept[f & ~base_bit] | (f & base_bit);
  }
  cmds_ = std::move(kept);
  compacted_size_ = cmds_.size();
}

HdImage HdList::materialize(int k) const {
  return materialize(k, 0, 0, width_, height_);
}

HdImage HdList::materialize(int k, int x, int y, int w, int h) const {
  HdImage out(w * k, h * k, 0u);
  const int ox = x * k;
  const int oy = y * k;
  for (std::size_t i = 0; i < cmds_.size(); ++i) {
    const HdCmd& c = cmds_[i];
    const int tx0 = std::max(c.dx, ox);
    const int ty0 = std::max(c.dy, oy);
    const int tx1 = std::min(c.dx + c.dw(), ox + out.width);
    const int ty1 = std::min(c.dy + c.dh(), oy + out.height);
    if (tx0 >= tx1 || ty0 >= ty1) {
      continue;
    }
    // Commands cover whole engine pixels, so the floor is looked up once
    // per engine pixel and applies to its k x k block of texels.
    for (int py = ty0 / k; py * k < ty1; ++py) {
      const int ya = std::max(ty0, py * k);
      const int yb = std::min(ty1, (py + 1) * k);
      for (int px = tx0 / k; px * k < tx1; ++px) {
        const uint32_t f = floor_of(px, py);
        if (i < f) {
          continue;
        }
        const int xa = std::max(tx0, px * k);
        const int xb = std::min(tx1, (px + 1) * k);
        for (int ty = ya; ty < yb; ++ty) {
          uint32_t* row = out.row(ty - oy);
          for (int tx = xa; tx < xb; ++tx) {
            const uint32_t v = sample(c, tx, ty);
            uint32_t& t = row[tx - ox];
            if (i == f) {
              t = alpha(v) != 0 ? v : 0u;
            }
            else if (alpha(v) != 0) {
              t = blend_over(v, t);
            }
          }
        }
      }
    }
  }
  return out;
}

} // namespace gfx
