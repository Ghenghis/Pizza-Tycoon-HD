/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The HD texel rules of gfx::Surface as they were before HD draw lists
// (Pizza Tycoon HD 1.0.0): every HD blit copied or blended texels on the CPU.
// Kept as the reference the draw-list implementation is checked against.

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <optional>
#include <vector>
#include "engine/gfx/HdImage.h"

namespace hdref {

using gfx::HdImage;

struct RefSurface {
  int w;
  int h;
  int k;
  std::vector<uint8_t> px;
  std::optional<HdImage> hd;

  RefSurface(int w_, int h_, int k_, uint8_t fill = 0)
      : w(w_), h(h_), k(k_), px(static_cast<std::size_t>(w_) * h_, fill) {}

  uint8_t get(int x, int y) const {
    return px[static_cast<std::size_t>(y) * w + x];
  }
  uint8_t& at(int x, int y) { return px[static_cast<std::size_t>(y) * w + x]; }

  HdImage& hd_mut() {
    if (!hd) {
      hd.emplace(w * k, h * k, 0u);
    }
    return *hd;
  }
  void clear_rect(int x0, int y0, int x1, int y1) {
    if (!hd || x1 <= x0 || y1 <= y0) {
      return;
    }
    for (int ty = y0 * k; ty < y1 * k; ++ty) {
      std::fill_n(hd->row(ty) + x0 * k, (x1 - x0) * k, 0u);
    }
  }
  void clear_pixel(int x, int y) { clear_rect(x, y, x + 1, y + 1); }

  void put(int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return;
    }
    at(x, y) = v;
    clear_pixel(x, y);
  }
  void fill(uint8_t v) {
    std::fill(px.begin(), px.end(), v);
    hd.reset();
  }
  void fill_rect(int rx, int ry, int rw, int rh, uint8_t v) {
    if (rw <= 0 || rh <= 0) {
      return;
    }
    const int x0 = std::max(0, rx);
    const int y0 = std::max(0, ry);
    const int x1 = std::min(w, rx + rw);
    const int y1 = std::min(h, ry + rh);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        at(x, y) = v;
      }
    }
    clear_rect(x0, y0, x1, y1);
  }

  void blit_raw(const RefSurface& src, int dx, int dy) {
    for (int sy = 0; sy < src.h; ++sy) {
      const int y = dy + sy;
      if (y < 0 || y >= h) {
        continue;
      }
      for (int sx = 0; sx < src.w; ++sx) {
        const int x = dx + sx;
        if (x >= 0 && x < w) {
          at(x, y) = src.get(sx, sy);
        }
      }
    }
    if (!src.hd && !hd) {
      return;
    }
    const int sx0 = std::max(0, -dx);
    const int sy0 = std::max(0, -dy);
    const int sx1 = std::min(src.w, w - dx);
    const int sy1 = std::min(src.h, h - dy);
    if (sx1 <= sx0 || sy1 <= sy0) {
      return;
    }
    if (!src.hd) {
      clear_rect(dx + sx0, dy + sy0, dx + sx1, dy + sy1);
      return;
    }
    const HdImage& from = *src.hd;
    HdImage& to = hd_mut();
    const auto count = static_cast<std::size_t>((sx1 - sx0) * k);
    for (int ty = sy0 * k; ty < sy1 * k; ++ty) {
      std::memcpy(to.row(ty + dy * k) + (dx + sx0) * k, from.row(ty) + sx0 * k,
                  count * sizeof(uint32_t));
    }
  }

  bool clip(const RefSurface& src, int& sx, int& sy, int& sw, int& sh, int& dx,
            int& dy) const {
    if (sx < 0) {
      dx -= sx;
      sw += sx;
      sx = 0;
    }
    if (sy < 0) {
      dy -= sy;
      sh += sy;
      sy = 0;
    }
    if (sx + sw > src.w) {
      sw = src.w - sx;
    }
    if (sy + sh > src.h) {
      sh = src.h - sy;
    }
    if (sw <= 0 || sh <= 0) {
      return false;
    }
    if (dx < 0) {
      sx -= dx;
      sw += dx;
      dx = 0;
    }
    if (dy < 0) {
      sy -= dy;
      sh += dy;
      dy = 0;
    }
    if (dx + sw > w) {
      sw = w - dx;
    }
    if (dy + sh > h) {
      sh = h - dy;
    }
    return sw > 0 && sh > 0;
  }

  void blit(const RefSurface& src, int sx, int sy, int sw, int sh, int dx,
            int dy, uint8_t transparent = 0) {
    if (!clip(src, sx, sy, sw, sh, dx, dy)) {
      return;
    }
    for (int r = 0; r < sh; ++r) {
      for (int c = 0; c < sw; ++c) {
        const uint8_t p = src.get(sx + c, sy + r);
        if (p != transparent) {
          at(dx + c, dy + r) = p;
        }
      }
    }
    if (!src.hd) {
      if (!hd) {
        return;
      }
      for (int r = 0; r < sh; ++r) {
        for (int c = 0; c < sw; ++c) {
          if (src.get(sx + c, sy + r) != transparent) {
            clear_pixel(dx + c, dy + r);
          }
        }
      }
      return;
    }
    const HdImage& from = *src.hd;
    HdImage& to = hd_mut();
    for (int r = 0; r < sh; ++r) {
      for (int ty = 0; ty < k; ++ty) {
        const uint32_t* from_row = from.row((sy + r) * k + ty) + sx * k;
        uint32_t* to_row = to.row((dy + r) * k + ty) + dx * k;
        for (int c = 0; c < sw; ++c) {
          const uint32_t* f = from_row + c * k;
          uint32_t* t = to_row + c * k;
          if (src.get(sx + c, sy + r) != transparent) {
            std::memcpy(t, f, static_cast<std::size_t>(k) * sizeof(uint32_t));
          }
          else {
            for (int tx = 0; tx < k; ++tx) {
              if (gfx::alpha(f[tx]) != 0) {
                t[tx] = gfx::blend_over(f[tx], t[tx]);
              }
            }
          }
        }
      }
    }
  }

  void blit_stencil(const RefSurface& src, int sx, int sy, int sw, int sh,
                    int dx, int dy, uint8_t color, uint8_t transparent = 0) {
    if (!clip(src, sx, sy, sw, sh, dx, dy)) {
      return;
    }
    for (int r = 0; r < sh; ++r) {
      for (int c = 0; c < sw; ++c) {
        if (src.get(sx + c, sy + r) != transparent) {
          at(dx + c, dy + r) = color;
          clear_pixel(dx + c, dy + r);
        }
      }
    }
  }

  void blit_half(const RefSurface& src, int sx, int sy, int sw, int sh, int dx,
                 int dy) {
    const int hw = sw / 2;
    const int hh = sh / 2;
    for (int y = 0; y < hh; ++y) {
      for (int x = 0; x < hw; ++x) {
        const int fx = sx + x * 2;
        const int fy = sy + y * 2;
        const int tx0 = dx + x;
        const int ty0 = dy + y;
        if (tx0 < 0 || ty0 < 0 || tx0 >= w || ty0 >= h || fx < 0 || fy < 0 ||
            fx >= src.w || fy >= src.h) {
          continue;
        }
        at(tx0, ty0) = src.get(fx, fy);
        if (!src.hd) {
          clear_pixel(tx0, ty0);
          continue;
        }
        HdImage& out = hd_mut();
        const HdImage& in = *src.hd;
        for (int ty = 0; ty < k; ++ty) {
          for (int tx = 0; tx < k; ++tx) {
            uint32_t rr = 0;
            uint32_t gg = 0;
            uint32_t bb = 0;
            uint32_t aa = 0;
            for (int yy = 0; yy < 2; ++yy) {
              for (int xx = 0; xx < 2; ++xx) {
                const uint32_t p =
                    in.at(fx * k + tx * 2 + xx, fy * k + ty * 2 + yy);
                const uint32_t pa = gfx::alpha(p);
                rr += gfx::red(p) * pa;
                gg += gfx::green(p) * pa;
                bb += gfx::blue(p) * pa;
                aa += pa;
              }
            }
            out.at(tx0 * k + tx, ty0 * k + ty) =
                aa == 0 ? 0u
                        : gfx::pack_rgba((rr + aa / 2) / aa, (gg + aa / 2) / aa,
                                         (bb + aa / 2) / aa, (aa + 2) / 4);
          }
        }
      }
    }
  }

  void replace_colors(std::initializer_list<uint8_t> keys,
                      uint8_t replacement) {
    if (*keys.begin() == replacement) {
      return;
    }
    std::array<uint8_t, 256> map{};
    std::iota(map.begin(), map.end(), 0);
    for (uint8_t key : keys) {
      map[key] = replacement;
    }
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const uint8_t before = at(x, y);
        at(x, y) = map[before];
        if (hd && at(x, y) != before) {
          clear_pixel(x, y);
        }
      }
    }
  }
};

} // namespace hdref
