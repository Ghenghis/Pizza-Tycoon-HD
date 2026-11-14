/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "engine/geom/Spaces.h"
#include "engine/gfx/Color.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Surface.h"

// HD pictures for the city view's map screens, drawn procedurally at
// gfx::hd_scale() texels per engine pixel. They go into the surfaces' HD
// draw lists only: the indexed pixels the game draws stay exactly as they
// are, and at hd_scale 1 nothing here does anything. Everything is plain art
// (no tint, no atlas), so the 1994 look (F7) hides it.
namespace views::hd {

// What a city tile is, for drawing the far map.
enum class MapTileClass : uint8_t {
  Ground,   // open land, parks, everything not below
  Water,    // drawn with smooth, anti-aliased coastlines
  Road,     // roads and paths: smooth, anti-aliased edges
  Building, // crisp blocks with a light top-left and a dark bottom-right edge
};

struct FarMapTile {
  uint8_t color = 0; // the palette index the indexed far map uses
  MapTileClass cls = MapTileClass::Ground;
};

// The far map's HD picture: `columns` x `rows` tiles of `tile_px` engine
// pixels each, so (columns * tile_px * k) x (rows * tile_px * k) texels.
// Each tile has its indexed colour (from `colors`, the palette the map is
// shown with, season included); water and roads get smooth edges, buildings
// clean bevelled edges, open land and water a faint texture. Opaque. Built
// once per distinct input (city, ownership, season, scale) and cached.
// Null when hd_scale() is 1 or the input does not fit.
[[nodiscard]] std::shared_ptr<const gfx::HdImage>
far_map_picture(std::span<const FarMapTile> tiles, int columns, int rows,
                int tile_px, const std::vector<gfx::Color>& colors);

// Attaches far_map_picture() to `map` (which must be columns * tile_px by
// rows * tile_px engine pixels). Leaves the indexed pixels alone. Returns
// false, and does nothing, at hd_scale 1.
bool attach_far_map(gfx::Surface& map, std::span<const FarMapTile> tiles,
                    int columns, int rows, int tile_px,
                    const std::vector<gfx::Color>& colors);

// The district and control tabs' hatching in HD: `cell` (engine pixels of
// `map`) is drawn as its base colour crossed by smooth diagonal stripes in
// the line colour, in the same places as the indexed 2x2 dots (lines where
// pattern x + y is a multiple of 8, the pattern starting at phase_x/phase_y
// at the cell's top-left pixel). Call after the cell's indexed pixels are
// drawn. Does nothing at hd_scale 1.
void hatch_cell(gfx::Surface& map, ViewRect cell, int phase_x, int phase_y,
                gfx::Color base, gfx::Color line);

// The picture hatch_cell() draws from: base colour, stripes every 8 engine
// pixels along x + y, `size_px` engine pixels square. Cached.
[[nodiscard]] std::shared_ptr<const gfx::HdImage>
hatch_picture(gfx::Color base, gfx::Color line, int size_px, int k);

} // namespace views::hd
