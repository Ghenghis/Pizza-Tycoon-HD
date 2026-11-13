/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/gfx/Surface.h"

using gfx::Surface;

namespace gfx::blit {

inline void blit_raw_with_palette(Surface& dst, const Surface& src,
                                  ViewPoint pt = {0, 0}) {
  dst.palette(src.palette());
  // Logger::info("blit_raw_with_palette {}", src.palette().id);
  dst.blit_raw(src, pt);
}
inline void blit_with_palette(Surface& dst, const Surface& src,
                              ViewPoint pt = {0, 0}) {
  dst.palette(src.palette());
  dst.blit_masked(src, pt);
}

// Copies src_rect at half size, keeping one pixel of each 2x2 block.
inline void blit_half_with_palette(Surface& dst, const Surface& src,
                                   ViewRect src_rect, ViewPoint pt = {0, 0}) {
  dst.palette(src.palette());
  dst.blit_half(src, src_rect, pt);
}

} // namespace gfx::blit
