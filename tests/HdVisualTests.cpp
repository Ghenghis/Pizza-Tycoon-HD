/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The procedural HD pictures of the views (src/views/hd): the city far map
// (H4), the district and control hatching (M2) and the newspaper's paper
// (N1). In every case the indexed pixels must stay byte for byte what the
// game drew, nothing may change at hd_scale 1, and the pictures must be art
// (no tint, no atlas) so the 1994 look hides them.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <vector>

#include "TestSupport.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Surface.h"
#include "views/hd/CityMapHd.h"
#include "views/hd/HdRestore.h"

using gfx::Color;
using gfx::HdImage;
using gfx::Surface;
using test_support::ScopedHdScale;
using views::hd::FarMapTile;
using views::hd::MapTileClass;

namespace {

constexpr int columns = 160;
constexpr int rows = 120;
constexpr int tile_px = 2;

constexpr uint8_t ground = 0x1C;
constexpr uint8_t water = 0x0D;
constexpr uint8_t road = 0x0E;
constexpr uint8_t building = 0x15;

// shift stands for another season: it changes the ground colour too, as the
// seasons do, since the far map only uses the colours its tiles have.
std::vector<Color> test_palette(uint8_t shift = 0) {
  std::vector<Color> pal(256);
  for (int i = 0; i < 256; ++i) {
    pal[static_cast<std::size_t>(i)] =
        Color{static_cast<uint8_t>((i * 37 + shift) & 0xFF),
              static_cast<uint8_t>((i * 91 + 40) & 0xFF),
              static_cast<uint8_t>((i * 53 + 90) & 0xFF)};
  }
  pal[ground] = {static_cast<uint8_t>(90 + shift), 150, 70};
  pal[water] = {40, 80, 190};
  pal[road] = {120, 120, 120};
  pal[building] = {200, 120, 90};
  return pal;
}

// A small city: a lake with a diagonal (staircase) shore in the top left, a
// road along row 60, a building block, open land elsewhere.
std::vector<FarMapTile> test_city() {
  std::vector<FarMapTile> tiles(static_cast<std::size_t>(columns) * rows);
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < columns; ++i) {
      FarMapTile t{ground, MapTileClass::Ground};
      if (i + j < 50) {
        t = {water, MapTileClass::Water};
      }
      if (j == 60) {
        t = {road, MapTileClass::Road};
      }
      if (i >= 100 && i < 110 && j >= 80 && j < 90) {
        t = {building, MapTileClass::Building};
      }
      tiles[static_cast<std::size_t>(j) * columns + i] = t;
    }
  }
  return tiles;
}

// The indexed far map exactly as CityView draws it: 2x2 pixels per tile.
Surface indexed_far_map(const std::vector<FarMapTile>& tiles) {
  Surface map(columns * tile_px, rows * tile_px);
  for (int i = 0; i < columns * rows; ++i) {
    map.fill_rect(ViewRect{(i % columns) * tile_px, (i / columns) * tile_px,
                           tile_px, tile_px},
                  tiles[static_cast<std::size_t>(i)].color);
  }
  return map;
}

std::vector<uint8_t> pixels_of(const Surface& s) {
  const auto p = s.pixels();
  return {p.begin(), p.end()};
}

uint32_t rgba(Color c) { return gfx::pack_rgba(c.r, c.g, c.b, 255); }

int channel_distance(uint32_t a, uint32_t b) {
  return std::abs(static_cast<int>(gfx::red(a)) -
                  static_cast<int>(gfx::red(b))) +
         std::abs(static_cast<int>(gfx::green(a)) -
                  static_cast<int>(gfx::green(b))) +
         std::abs(static_cast<int>(gfx::blue(a)) -
                  static_cast<int>(gfx::blue(b)));
}

// Every HD command on the surface is art: no tint, no atlas (so the 1994
// look, which draws only text, hides it).
bool all_art(const Surface& s) {
  const gfx::HdList* list = s.hd_list();
  if (!list) {
    return true;
  }
  for (const gfx::HdCmd& c : list->cmds()) {
    if (c.tint != 0 || (c.img && c.img->atlas)) {
      return false;
    }
  }
  return true;
}

// The district hatching of CityView::draw_cell_with_pattern, indexed part:
// 2x2 dots of `line` where pattern y is even and pattern x % 8 = 6 - y % 8.
void indexed_hatch(Surface& map, ViewRect cell, int phase_x, int phase_y,
                   uint8_t base, uint8_t line) {
  map.fill_rect(cell, base);
  for (int y = 0; y < cell.h; ++y) {
    for (int x = 0; x < cell.w; ++x) {
      const int px = x + phase_x;
      const int py = y + phase_y;
      if (py % 2 == 0 && px % 8 == 6 - py % 8) {
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            map.put(ViewPoint{cell.x + x + dx, cell.y + y + dy}, line);
          }
        }
      }
    }
  }
}

} // namespace

TEST_CASE("Far map: at hd_scale 1 nothing changes", "[hd][visual]") {
  ScopedHdScale scale(1);
  const auto tiles = test_city();
  Surface map = indexed_far_map(tiles);
  const auto before = pixels_of(map);
  CHECK_FALSE(views::hd::attach_far_map(map, tiles, columns, rows, tile_px,
                                        test_palette()));
  CHECK_FALSE(map.has_hd());
  CHECK(pixels_of(map) == before);
  CHECK(views::hd::far_map_picture(tiles, columns, rows, tile_px,
                                   test_palette()) == nullptr);
}

TEST_CASE("Far map: at hd_scale 2 it has an HD picture and the same pixels",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  const auto pal = test_palette();
  const auto tiles = test_city();
  Surface map = indexed_far_map(tiles);
  const auto before = pixels_of(map);

  REQUIRE(views::hd::attach_far_map(map, tiles, columns, rows, tile_px, pal));
  REQUIRE(map.has_hd());
  CHECK(pixels_of(map) == before);
  CHECK(all_art(map));

  const HdImage* hd = map.hd();
  REQUIRE(hd != nullptr);
  REQUIRE(hd->width == columns * tile_px * k);
  REQUIRE(hd->height == rows * tile_px * k);
  // Opaque: the indexed pixels never show through.
  bool opaque = true;
  for (uint32_t texel : hd->pixels) {
    opaque = opaque && gfx::alpha(texel) == 255;
  }
  CHECK(opaque);

  // Inside the building block: exactly the tile's colour (clean, no grain).
  const int t = tile_px * k; // texels per tile
  CHECK(hd->at(105 * t + 1, 85 * t + 1) == rgba(pal[building]));
  // Open land far from anything: the land colour with a faint grain.
  CHECK(channel_distance(hd->at(140 * t + 1, 20 * t + 1), rgba(pal[ground])) <=
        3 * 12);
  // The block's bottom edge is shaded darker, its top edge lighter.
  CHECK(gfx::red(hd->at(105 * t + 1, 90 * t - 1)) < pal[building].r);
  CHECK(gfx::red(hd->at(105 * t + 1, 80 * t)) > pal[building].r);

  // The staircase shore becomes a smooth, anti-aliased line: colours
  // between land and water appear along it.
  std::set<uint32_t> shore;
  for (int y = 0; y < 50 * t; ++y) {
    const int x = 50 * t - y; // along the diagonal i + j = 50
    for (int dx = -t; dx <= t; ++dx) {
      const int xx = x + dx;
      if (xx >= 0 && xx < hd->width) {
        const uint32_t c = hd->at(xx, y);
        const int to_land = channel_distance(c, rgba(pal[ground]));
        const int to_water = channel_distance(c, rgba(pal[water]));
        if (to_land > 40 && to_water > 40) {
          shore.insert(c);
        }
      }
    }
  }
  CHECK(shore.size() > 5);
}

TEST_CASE("Far map: one picture per city, season and scale", "[hd][visual]") {
  ScopedHdScale scale(2);
  const auto tiles = test_city();
  const auto a =
      views::hd::far_map_picture(tiles, columns, rows, tile_px, test_palette());
  const auto b =
      views::hd::far_map_picture(tiles, columns, rows, tile_px, test_palette());
  const auto winter = views::hd::far_map_picture(tiles, columns, rows, tile_px,
                                                 test_palette(17));
  REQUIRE(a != nullptr);
  CHECK(a == b); // the cached picture, no rebuild
  CHECK(winter != a);
  CHECK(views::hd::far_map_picture(std::span<const FarMapTile>(tiles).first(10),
                                   columns, rows, tile_px,
                                   test_palette()) == nullptr);
}

TEST_CASE("Hatching: at hd_scale 1 nothing changes", "[hd][visual]") {
  ScopedHdScale scale(1);
  Surface map(64, 64);
  const ViewRect cell{0, 0, 16, 16};
  indexed_hatch(map, cell, 3, 5, 15, 240);
  const auto before = pixels_of(map);
  views::hd::hatch_cell(map, cell, 3, 5, Color{200, 180, 120}, Color{0, 0, 0});
  CHECK_FALSE(map.has_hd());
  CHECK(pixels_of(map) == before);
}

TEST_CASE("Hatching: at hd_scale 2 smooth stripes over the same pixels",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  const Color base{200, 180, 120};
  const Color line{10, 10, 10};
  Surface map(80, 80);
  const ViewRect cells[] = {{0, 0, 16, 16}, {16, 0, 16, 16}, {0, 16, 64, 64}};
  const int phases[][2] = {{0, 0}, {5, 3}, {7, 6}};
  for (int n = 0; n < 3; ++n) {
    indexed_hatch(map, cells[n], phases[n][0], phases[n][1], 15, 240);
  }
  const auto before = pixels_of(map);
  for (int n = 0; n < 3; ++n) {
    views::hd::hatch_cell(map, cells[n], phases[n][0], phases[n][1], base,
                          line);
  }
  REQUIRE(map.has_hd());
  CHECK(pixels_of(map) == before);
  CHECK(all_art(map));
  CHECK(map.hd_list()->cmds().size() == 3); // one quad per cell

  const HdImage* hd = map.hd();
  REQUIRE(hd != nullptr);
  for (int n = 0; n < 3; ++n) {
    const ViewRect& cell = cells[n];
    for (int y = 0; y + 1 < cell.h; ++y) {
      for (int x = 0; x + 1 < cell.w; ++x) {
        const int px = x + phases[n][0];
        const int py = y + phases[n][1];
        const int tx = (cell.x + x + 1) * k; // texel at the dot's centre
        const int ty = (cell.y + y + 1) * k;
        if (py % 2 == 0 && px % 8 == 6 - py % 8) {
          // Where the indexed map has a dot, the stripe runs through.
          CHECK(channel_distance(hd->at(tx, ty), rgba(line)) < 60);
        }
        if ((px + py + 2) % 8 == 4) {
          // Half way between two lines: plain base colour.
          CHECK(hd->at(tx, ty) == rgba(base));
        }
      }
    }
  }
}

TEST_CASE("Paper: at hd_scale 1 nothing changes", "[hd][visual]") {
  ScopedHdScale scale(1);
  Surface paper(40, 30, std::nullopt, 159);
  Surface dst(40, 30);
  dst.blit_raw(paper);
  CHECK(views::hd::restore_hd_where(dst, paper, ViewRect{0, 0, 40, 30},
                                    ViewPoint{0, 0}, 159) == 0);
  CHECK_FALSE(dst.has_hd());
}

TEST_CASE("Paper: a cleared area gets its HD paper back, not erased content",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  // The paper: index 159 with a printed block (index 7) in the middle, and
  // an HD picture with a texture.
  Surface paper(40, 30, std::nullopt, 159);
  paper.fill_rect(ViewRect{18, 12, 4, 3}, 7);
  auto img = std::make_shared<HdImage>(40 * k, 30 * k);
  for (int y = 0; y < img->height; ++y) {
    for (int x = 0; x < img->width; ++x) {
      img->at(x, y) =
          gfx::pack_rgba(200 + (x * 7 + y * 3) % 40, 190 + (x * 5) % 30,
                         150 + (y * 11) % 50, 255);
    }
  }
  paper.attach_hd(img);

  Surface screen(40, 30);
  screen.blit_raw(paper);
  const ViewRect area{5, 5, 30, 20};
  screen.fill_rect(area, 159); // what the newspaper does
  const auto before = pixels_of(screen);

  CHECK(views::hd::restore_hd_where(screen, paper, area, area.point(), 159) >
        0);
  CHECK(pixels_of(screen) == before);
  CHECK(all_art(screen));

  const HdImage* hd = screen.hd();
  REQUIRE(hd != nullptr);
  for (int y = area.y; y < area.y + area.h; ++y) {
    for (int x = area.x; x < area.x + area.w; ++x) {
      const bool near_print = x >= 17 && x <= 22 && y >= 11 && y <= 15;
      const uint32_t texel = hd->at(x * k, y * k);
      if (near_print) {
        // The erased print and the paper around it stay plain: no ghosts.
        CHECK(gfx::alpha(texel) == 0);
      }
      else {
        CHECK(texel == img->at(x * k, y * k));
      }
    }
  }
  // Outside the cleared area the paper was never touched.
  CHECK(hd->at(1, 1) == img->at(1, 1));
}

TEST_CASE("Paper: letters drawn after the paper stay on top", "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  Surface paper(20, 10, std::nullopt, 159);
  paper.attach_hd(std::make_shared<HdImage>(
      20 * k, 10 * k, gfx::pack_rgba(220, 210, 180, 255)));
  Surface screen(20, 10);
  screen.blit_raw(paper);
  screen.fill_rect(ViewRect{0, 0, 20, 10}, 159);
  views::hd::restore_hd_where(screen, paper, ViewRect{0, 0, 20, 10},
                              ViewPoint{0, 0}, 159);
  // An HD letter, as HD text draws it: a tinted overlay after the paper.
  gfx::HdCmd letter;
  letter.img =
      std::make_shared<HdImage>(4, 4, gfx::pack_rgba(255, 255, 255, 255));
  letter.sw = 4;
  letter.sh = 4;
  letter.dx = 8;
  letter.dy = 8;
  letter.tint = gfx::pack_rgba(0, 0, 0, 255);
  screen.hd_overlay(letter, 0, 0, 20 * k, 10 * k);
  const HdImage* hd = screen.hd();
  REQUIRE(hd != nullptr);
  CHECK(hd->at(9, 9) == gfx::pack_rgba(0, 0, 0, 255));
  CHECK(hd->at(2, 2) == gfx::pack_rgba(220, 210, 180, 255));
}

TEST_CASE("Paper: fill and keyed blit give the same pixels as blit_raw",
          "[hd][visual]") {
  // The newspaper's HD path replaces blit_raw(text) by fill_rect(paper
  // colour) + blit(text, key = paper colour). The indexed result must be the
  // same everywhere, clipping included.
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> pick(0, 3);
  for (const ViewPoint at :
       {ViewPoint{3, 4}, ViewPoint{-5, -2}, ViewPoint{30, 20}}) {
    Surface text(16, 12, std::nullopt, 159);
    for (int y = 0; y < 12; ++y) {
      for (int x = 0; x < 16; ++x) {
        const uint8_t choices[] = {159, 240, 0, 17};
        text.put(ViewPoint{x, y}, choices[pick(rng)]);
      }
    }
    Surface a(40, 30, std::nullopt, 33);
    Surface b(40, 30, std::nullopt, 33);
    a.blit_raw(text, at);
    b.fill_rect(ViewRect{at.x, at.y, text.width(), text.height()}, 159);
    b.blit(text, text.rect(), at, 159);
    CHECK(pixels_of(a) == pixels_of(b));
  }
}
