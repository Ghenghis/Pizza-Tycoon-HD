/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
#include "engine/gfx/HdImage.h"
#include "engine/gfx/PaletteHandle.h"
#include "engine/gfx/Surface.h"
#include "views/hd/BackgroundJob.h"

// The city view's MEDIUM zoom map in HD (hd-audit M6). Its tiles come from
// gfx/med1, 4x4 original pixels each: too small for any HD pack to give more
// than a smudge. The same tiles exist in gfx/landsym at 16x16 original
// pixels (the CLOSE zoom's), so in HD the medium tiles are made from HD
// landsym, area-averaged down to med1's size (192 -> 48 texels per tile at
// 4K). The indexed pixels are med1's, exactly as before; the HD commands are
// plain art (no tint, no atlas), so the 1994 look hides them; at hd_scale 1
// nothing here does anything.
namespace views::hd {

// Draws the indexed medium map: tile i of `types` (row-major, `columns` per
// row) is the 8x8 tile of `tiles` for that type, at (i % columns,
// i / columns) * 8. This is CityView's loop; the HD build below uses it too,
// so both give the same pixels. False, part drawn, when `stop` was asked.
bool draw_medium_map(gfx::Surface& map, const gfx::Surface& tiles,
                     std::span<const uint8_t> types, int columns,
                     std::stop_token stop = {});

// med1-sized sheet made from HD landsym: the landsym picture (landsym_w x
// landsym_h engine pixels, so landsym_w * k texels wide) shrunk by
// landsym_w / tiles_w with an alpha-correct area average. Both sheets have
// the same tile layout, so that is a 4:1 average per tile. Cached in the
// map picture cache. Null when the sizes do not fit or `stop` was asked.
[[nodiscard]] std::shared_ptr<const gfx::HdImage>
medium_tile_sheet(const std::shared_ptr<const gfx::HdImage>& landsym_hd,
                  int landsym_w, int landsym_h, int tiles_w, int tiles_h, int k,
                  std::stop_token stop = {});

// What the worker needs, copied on the game thread.
struct MediumMapInput {
  std::vector<uint8_t> tile_pixels; // med1's indexed pixels
  int tiles_w = 0;                  // med1's size, engine pixels
  int tiles_h = 0;
  gfx::PaletteHandle tiles_palette{0};
  std::shared_ptr<const gfx::HdImage> landsym_hd; // shared, never written
  int landsym_w = 0;
  int landsym_h = 0;
  std::vector<uint8_t> types; // the city's tiles, row-major
  int columns = 0;
  int rows = 0;
  gfx::PaletteHandle map_palette{0};
  int k = 1;
};

// The input for the map, or nothing at hd_scale 1, without an HD landsym
// (no pack), or when the sizes do not fit.
[[nodiscard]] std::optional<MediumMapInput>
medium_map_input(const gfx::Surface& tiles, const gfx::Surface& landsym,
                 std::span<const uint8_t> types, int columns, int rows,
                 gfx::PaletteHandle map_palette);

// The whole medium map, built on the calling thread: indexed pixels as
// draw_medium_map() with med1, HD from medium_tile_sheet(). Null when
// stopped, or when hd_scale() is no longer input.k.
[[nodiscard]] std::shared_ptr<gfx::Surface>
build_medium_map(const MediumMapInput& input, std::stop_token stop = {});

using SurfaceJob = BackgroundJob<gfx::Surface>;

// Starts build_medium_map() on `job`'s worker (keyed on the input; a job
// for other input is stopped and its map dropped). Take the map with
// job.take() on a later frame and use it in place of the indexed one.
void start_medium_map(SurfaceJob& job, MediumMapInput input);

} // namespace views::hd
