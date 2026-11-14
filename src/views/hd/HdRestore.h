/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include "engine/geom/Spaces.h"
#include "engine/gfx/Surface.h"

namespace views::hd {

// Puts the HD picture of `src` back over `dst`, wherever the original only
// cleared to a plain colour: for every pixel of src_rect whose indexed value
// in src is `key` (and so looks the same as the clear), src's HD texels are
// drawn over dst at dst_pos + (that pixel's offset in src_rect). Pixels of
// another value (content the original meant to erase) are left as they are,
// and so are their neighbours, whose HD texels may hold that content's soft
// edges.
//
// Only dst's HD draw list changes, never its indexed pixels; drawn as art
// (no tint, no atlas), so the 1994 look hides it. Call it after the clear
// and before whatever is drawn on top. Does nothing at hd_scale 1 or when
// src has no HD picture. Returns how many HD commands it added.
int restore_hd_where(gfx::Surface& dst, const gfx::Surface& src,
                     ViewRect src_rect, ViewPoint dst_pos, uint8_t key);

} // namespace views::hd
