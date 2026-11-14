/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/hd/HdRestore.h"

#include <algorithm>
#include <memory>
#include <vector>
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"

namespace views::hd {

int restore_hd_where(gfx::Surface& dst, const gfx::Surface& src,
                     ViewRect src_rect, ViewPoint dst_pos, uint8_t key) {
  const int k = gfx::hd_scale();
  const gfx::HdList* list = src.hd_list();
  if (k <= 1 || !list) {
    return 0;
  }
  const int x0 = std::max(src_rect.x, 0);
  const int y0 = std::max(src_rect.y, 0);
  const int x1 = std::min(src_rect.x + src_rect.w, src.width());
  const int y1 = std::min(src_rect.y + src_rect.h, src.height());
  if (x1 <= x0 || y1 <= y0) {
    return 0;
  }
  // Where src's engine pixel (x0, y0) lands on dst.
  const int to_x = dst_pos.x + (x0 - src_rect.x);
  const int to_y = dst_pos.y + (y0 - src_rect.y);

  // The picture to copy from: a single picture's own image (no copy), or
  // the rect flattened once.
  std::shared_ptr<const gfx::HdImage> img;
  int img_x = 0; // texel of img for src engine pixel x0
  int img_y = 0;
  if (list->is_leaf() && !list->cmds().front().half) {
    const gfx::HdCmd& leaf = list->cmds().front();
    img = leaf.img;
    img_x = leaf.sx + x0 * k - leaf.dx;
    img_y = leaf.sy + y0 * k - leaf.dy;
  }
  else {
    img = std::make_shared<const gfx::HdImage>(
        list->materialize(k, x0, y0, x1 - x0, y1 - y0));
  }

  // A pixel is put back when it and its eight neighbours are `key`: the HD
  // art of anything the clear erases has soft edges that reach into the
  // paper next to it, and those must not come back as ghosts.
  const auto clean = [&](int x, int y) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const ViewPoint p{x + dx, y + dy};
        if (p.x >= 0 && p.y >= 0 && p.x < src.width() && p.y < src.height() &&
            src.get(p) != key) {
          return false;
        }
      }
    }
    return true;
  };

  int pushed = 0;
  // A rect [rx0, rx1) x [ry0, ry1) of src engine pixels, all clean.
  const auto overlay = [&](int rx0, int rx1, int ry0, int ry1) {
    gfx::HdCmd cmd;
    cmd.img = img;
    cmd.sx = img_x + (rx0 - x0) * k;
    cmd.sy = img_y + (ry0 - y0) * k;
    cmd.sw = (rx1 - rx0) * k;
    cmd.sh = (ry1 - ry0) * k;
    cmd.dx = (to_x + rx0 - x0) * k;
    cmd.dy = (to_y + ry0 - y0) * k;
    const int cx = cmd.dx;
    const int cy = cmd.dy;
    const int cw = cmd.sw;
    const int ch = cmd.sh;
    dst.hd_overlay(std::move(cmd), cx, cy, cx + cw, cy + ch);
    ++pushed;
  };

  // Runs of clean pixels per row, merged downwards while a run repeats
  // exactly, so a plain area is one command.
  struct Run {
    int x0, x1, y0;
  };
  std::vector<Run> open;
  std::vector<Run> next;
  std::vector<bool> continued;
  for (int y = y0; y <= y1; ++y) {
    next.clear();
    continued.assign(open.size(), false);
    if (y < y1) {
      int x = x0;
      while (x < x1) {
        while (x < x1 && !clean(x, y)) {
          ++x;
        }
        if (x >= x1) {
          break;
        }
        const int start = x;
        while (x < x1 && clean(x, y)) {
          ++x;
        }
        auto it = std::find_if(open.begin(), open.end(), [&](const Run& r) {
          return r.x0 == start && r.x1 == x;
        });
        if (it != open.end()) {
          continued[static_cast<std::size_t>(it - open.begin())] = true;
          next.push_back(*it);
        }
        else {
          next.push_back(Run{start, x, y});
        }
      }
    }
    for (std::size_t i = 0; i < open.size(); ++i) {
      if (!continued[i]) {
        overlay(open[i].x0, open[i].x1, open[i].y0, y);
      }
    }
    open.swap(next);
  }
  return pushed;
}

} // namespace views::hd
