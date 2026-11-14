/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/hd/MediumMapHd.h"

#include <cstddef>
#include <utility>
#include "core/util/Constants.h"
#include "engine/gfx/HdList.h"
#include "views/hd/PictureCache.h"

namespace views::hd {

namespace {

using LegacyConstants::BASE_MAP_TILE_HEIGHT;
using LegacyConstants::BASE_MAP_TILE_WIDTH;

// Shrinks by an integer factor f: each texel is the average of an f x f
// block, weighted by alpha (premultiplied), so transparent texels give no
// colour. Exact integer maths. Empty when stopped.
gfx::HdImage shrink(const gfx::HdImage& src, int f, std::stop_token stop) {
  const int w = src.width / f;
  const int h = src.height / f;
  gfx::HdImage out(w, h);
  const uint32_t n = static_cast<uint32_t>(f) * static_cast<uint32_t>(f);
  for (int y = 0; y < h; ++y) {
    if (stop.stop_requested()) {
      return {};
    }
    uint32_t* dst = out.row(y);
    for (int x = 0; x < w; ++x) {
      uint32_t sa = 0;
      uint32_t sr = 0;
      uint32_t sg = 0;
      uint32_t sb = 0;
      for (int dy = 0; dy < f; ++dy) {
        const uint32_t* s =
            src.row(y * f + dy) + static_cast<std::size_t>(x) * f;
        for (int dx = 0; dx < f; ++dx) {
          const uint32_t p = s[dx];
          const uint32_t a = gfx::alpha(p);
          sa += a;
          sr += gfx::red(p) * a;
          sg += gfx::green(p) * a;
          sb += gfx::blue(p) * a;
        }
      }
      if (sa == 0) {
        dst[x] = 0;
        continue;
      }
      // Colour: the alpha-weighted mean; alpha: the plain mean. Rounded.
      dst[x] = gfx::pack_rgba((sr + sa / 2) / sa, (sg + sa / 2) / sa,
                              (sb + sa / 2) / sa, (sa + n / 2) / n);
    }
  }
  return out;
}

bool sheet_fits(int landsym_w, int landsym_h, int tiles_w, int tiles_h) {
  if (tiles_w <= 0 || tiles_h <= 0 || landsym_w <= 0 || landsym_h <= 0 ||
      landsym_w % tiles_w != 0 || landsym_h % tiles_h != 0) {
    return false;
  }
  const int f = landsym_w / tiles_w;
  return f >= 1 && landsym_h / tiles_h == f;
}

uint64_t sheet_key(const gfx::HdImage& landsym_hd, int landsym_w, int landsym_h,
                   int tiles_w, int tiles_h, int k) {
  KeyHash key;
  key.add(uint32_t{0x6d656431}); // "med1": not a far map's key
  add_picture_identity(key, landsym_hd);
  key.add(landsym_w);
  key.add(landsym_h);
  key.add(tiles_w);
  key.add(tiles_h);
  key.add(k);
  return key.value();
}

} // namespace

bool draw_medium_map(gfx::Surface& map, const gfx::Surface& tiles,
                     std::span<const uint8_t> types, int columns,
                     std::stop_token stop) {
  if (columns <= 0) {
    return true;
  }
  for (std::size_t i = 0; i < types.size(); ++i) {
    const int n = static_cast<int>(i);
    if (n % columns == 0 && stop.stop_requested()) {
      return false;
    }
    const ViewPoint pos{(n % columns) * BASE_MAP_TILE_WIDTH,
                        (n / columns) * BASE_MAP_TILE_HEIGHT};
    map.blit(tiles, landsym_source_rect(types[i]), pos);
  }
  return true;
}

std::shared_ptr<const gfx::HdImage>
medium_tile_sheet(const std::shared_ptr<const gfx::HdImage>& landsym_hd,
                  int landsym_w, int landsym_h, int tiles_w, int tiles_h, int k,
                  std::stop_token stop) {
  if (k <= 1 || !landsym_hd ||
      !sheet_fits(landsym_w, landsym_h, tiles_w, tiles_h) ||
      landsym_hd->width != landsym_w * k ||
      landsym_hd->height != landsym_h * k) {
    return nullptr;
  }
  const uint64_t id =
      sheet_key(*landsym_hd, landsym_w, landsym_h, tiles_w, tiles_h, k);
  if (auto hit = map_picture_cache().find(id)) {
    return hit;
  }
  gfx::HdImage sheet = shrink(*landsym_hd, landsym_w / tiles_w, stop);
  if (sheet.width == 0) {
    return nullptr; // stopped
  }
  auto img = std::make_shared<const gfx::HdImage>(std::move(sheet));
  prefill_opacity(*img, k);
  return map_picture_cache().add(id, std::move(img));
}

std::optional<MediumMapInput> medium_map_input(const gfx::Surface& tiles,
                                               const gfx::Surface& landsym,
                                               std::span<const uint8_t> types,
                                               int columns, int rows,
                                               gfx::PaletteHandle map_palette) {
  const int k = gfx::hd_scale();
  const gfx::HdList* list = landsym.hd_list();
  if (k <= 1 || !list || !list->is_leaf() || columns <= 0 || rows <= 0 ||
      types.size() != static_cast<std::size_t>(columns) * rows ||
      !sheet_fits(landsym.width(), landsym.height(), tiles.width(),
                  tiles.height())) {
    return std::nullopt;
  }
  MediumMapInput in;
  const auto px = tiles.pixels();
  in.tile_pixels.assign(px.begin(), px.end());
  in.tiles_w = tiles.width();
  in.tiles_h = tiles.height();
  in.tiles_palette = tiles.palette();
  in.landsym_hd = list->cmds().front().img;
  in.landsym_w = landsym.width();
  in.landsym_h = landsym.height();
  in.types.assign(types.begin(), types.end());
  in.columns = columns;
  in.rows = rows;
  in.map_palette = map_palette;
  in.k = k;
  if (!in.landsym_hd || in.landsym_hd->width != in.landsym_w * k ||
      in.landsym_hd->height != in.landsym_h * k) {
    return std::nullopt;
  }
  return in;
}

std::shared_ptr<gfx::Surface> build_medium_map(const MediumMapInput& in,
                                               std::stop_token stop) {
  // Surfaces read hd_scale() when they draw; it is set once at startup.
  if (in.k <= 1 || gfx::hd_scale() != in.k) {
    return nullptr;
  }
  auto sheet = medium_tile_sheet(in.landsym_hd, in.landsym_w, in.landsym_h,
                                 in.tiles_w, in.tiles_h, in.k, stop);
  if (!sheet) {
    return nullptr;
  }
  // A private med1 with the sheet as its HD picture: nothing here is shared
  // with the game thread except the (read-only) pictures.
  gfx::Surface tiles = gfx::Surface::from_indexed(
      in.tile_pixels, in.tiles_w, in.tiles_h, in.tiles_palette, 1);
  tiles.attach_hd(std::move(sheet));
  auto map = std::make_shared<gfx::Surface>(
      Size{in.columns * BASE_MAP_TILE_WIDTH, in.rows * BASE_MAP_TILE_HEIGHT},
      in.map_palette);
  if (!draw_medium_map(*map, tiles, in.types, in.columns, stop)) {
    return nullptr;
  }
  return map;
}

void start_medium_map(SurfaceJob& job, MediumMapInput input) {
  KeyHash key;
  key.add(uint32_t{0x6d617031}); // "map1"
  key.add(input.landsym_hd.get());
  key.add(input.tile_pixels.data(), input.tile_pixels.size());
  key.add(input.types.data(), input.types.size());
  key.add(input.columns);
  key.add(input.map_palette);
  key.add(input.k);
  job.start(key.value(), [in = std::move(input)](std::stop_token stop) {
    return build_medium_map(in, stop);
  });
}

} // namespace views::hd
