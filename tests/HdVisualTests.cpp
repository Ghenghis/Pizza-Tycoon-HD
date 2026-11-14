/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The procedural HD pictures of the views (src/views/hd): the city far map
// (H4, built off the frame), the medium zoom map (M6), the water cycle at
// close zoom (M4), the district and control hatching (M2) and the
// newspaper's paper (N1). In every case the indexed pixels must stay byte
// for byte what the game drew, nothing may change at hd_scale 1, and the
// pictures must be art (no tint, no atlas) so the 1994 look hides them.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "core/util/Constants.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Surface.h"
#include "views/hd/BackgroundJob.h"
#include "views/hd/CityMapHd.h"
#include "views/hd/HdRestore.h"
#include "views/hd/MediumMapHd.h"
#include "views/hd/WaterCycleHd.h"

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

// Polls a BackgroundJob the way a view does, once per "frame", until its
// result is there, it ended without one, or two minutes passed.
template <typename Job> auto wait_for(Job& job) {
  using clock = std::chrono::steady_clock;
  const auto until = clock::now() + std::chrono::minutes(2);
  auto result = job.take();
  while (!result && job.started() && clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    result = job.take();
  }
  return result;
}

// A picture for `s` where each engine pixel's k x k block is the colour of
// its index in `pal`: what an HD pack exported in that palette looks like,
// minus the detail.
std::shared_ptr<HdImage> palette_picture(const Surface& s,
                                         const std::vector<Color>& pal, int k) {
  auto img = std::make_shared<HdImage>(s.width() * k, s.height() * k);
  for (int y = 0; y < img->height; ++y) {
    for (int x = 0; x < img->width; ++x) {
      img->at(x, y) = rgba(pal[s.get(ViewPoint{x / k, y / k})]);
    }
  }
  return img;
}

// The water of the city view (CityView::setup_water_animation).
const std::vector<uint8_t> water_indices{140, 141, 142, 143};
const std::vector<std::vector<uint8_t>> water_cycles{
    {140, 141, 142, 143},
    {141, 142, 143, 140},
    {142, 143, 140, 141},
    {143, 140, 141, 142},
};

std::vector<Color> water_palette() {
  auto pal = test_palette();
  pal[28] = {60, 120, 40};
  pal[140] = {20, 40, 160};
  pal[141] = {30, 60, 180};
  pal[142] = {40, 80, 200};
  pal[143] = {0, 100, 220}; // a channel at 0, to cover that case
  return pal;
}

// A landsym-shaped sheet (512x512 engine pixels, 16x16 tiles of 32): land
// (index 28) everywhere, but tile 5 has water stripes (140-143) in its left
// half and tile 33 one water pixel.
Surface water_landsym() {
  Surface s(512, 512, std::nullopt, 28);
  const ViewRect five = landsym_source_rect(5, BASE_MAP_ZOOM_MULTIPLIER);
  for (int y = 0; y < five.h; ++y) {
    for (int x = 0; x < five.w / 2; ++x) {
      s.put(ViewPoint{five.x + x, five.y + y},
            static_cast<uint8_t>(140 + (x + y) % 4));
    }
  }
  const ViewRect t33 = landsym_source_rect(33, BASE_MAP_ZOOM_MULTIPLIER);
  s.put(ViewPoint{t33.x + 3, t33.y + 4}, 142);
  return s;
}

// A med1-shaped sheet (128x128, 16x16 tiles of 8) and a landsym-shaped one
// (512x512) for it: tile t is index t + 1 in med1; in HD landsym tile t is
// one colour, except tile 3, a checker of 2x2-texel black and white blocks.
Color tile_colour(int t) {
  return Color{static_cast<uint8_t>(t), static_cast<uint8_t>(255 - t),
               static_cast<uint8_t>((t * 7) & 0xFF)};
}

Surface medium_tiles() {
  Surface s(128, 128);
  for (int t = 0; t < 256; ++t) {
    s.fill_rect(landsym_source_rect(static_cast<uint8_t>(t)),
                static_cast<uint8_t>(t == 255 ? 1 : t + 1));
  }
  return s;
}

Surface checker_landsym(int k) {
  Surface s(512, 512, std::nullopt, 9);
  auto img = std::make_shared<HdImage>(512 * k, 512 * k);
  for (int t = 0; t < 256; ++t) {
    const ViewRect r =
        landsym_source_rect(static_cast<uint8_t>(t), BASE_MAP_ZOOM_MULTIPLIER);
    for (int y = r.y * k; y < (r.y + r.h) * k; ++y) {
      for (int x = r.x * k; x < (r.x + r.w) * k; ++x) {
        uint32_t c = rgba(tile_colour(t));
        if (t == 3) {
          c = ((x / 2 + y / 2) % 2) ? gfx::pack_rgba(255, 255, 255, 255)
                                    : gfx::pack_rgba(0, 0, 0, 255);
        }
        img->at(x, y) = c;
      }
    }
  }
  s.attach_hd(img);
  return s;
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

TEST_CASE("Background job: the result arrives later, once", "[hd][visual]") {
  std::atomic<bool> go{false};
  std::atomic<int> runs{0};
  views::hd::BackgroundJob<int> job;
  CHECK_FALSE(job.started());
  auto work = [&](std::stop_token) {
    ++runs;
    while (!go) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::make_shared<int>(7);
  };
  job.start(1, work);
  job.start(1, work); // the same key: no second run
  CHECK(job.started());
  CHECK(job.take() == nullptr); // still working
  go = true;
  const auto result = wait_for(job);
  REQUIRE(result != nullptr);
  CHECK(*result == 7);
  CHECK(job.take() == nullptr); // handed over once
  CHECK_FALSE(job.started());
  CHECK(runs == 1);
}

TEST_CASE("Background job: a result for an old key is never handed over",
          "[hd][visual]") {
  std::atomic<bool> go{false};
  views::hd::BackgroundJob<int> job;
  // Key 1 ignores its stop token and finishes anyway, after key 2 started.
  job.start(1, [&](std::stop_token) {
    while (!go) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::make_shared<int>(1);
  });
  job.start(2, [&](std::stop_token) {
    while (!go) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::make_shared<int>(2);
  });
  go = true;
  const auto result = wait_for(job);
  REQUIRE(result != nullptr);
  CHECK(*result == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(job.take() == nullptr);
}

TEST_CASE("Background job: stopping ends the work early", "[hd][visual]") {
  std::atomic<bool> stopped{false};
  {
    views::hd::BackgroundJob<int> job;
    job.start(1, [&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      stopped = true;
      return std::make_shared<int>(1);
    });
    job.cancel();
    CHECK_FALSE(job.started());
    CHECK(job.take() == nullptr);
  } // joined here
  CHECK(stopped);
}

TEST_CASE("Far map off the frame: at hd_scale 1 no picture and no job",
          "[hd][visual]") {
  ScopedHdScale scale(1);
  const auto tiles = test_city();
  views::hd::PictureJob job;
  CHECK(views::hd::far_map_picture_soon(job, tiles, columns, rows, tile_px,
                                        test_palette()) == nullptr);
  CHECK_FALSE(job.started());
  Surface map = indexed_far_map(tiles);
  CHECK_FALSE(views::hd::attach_far_map_picture(map, nullptr));
  CHECK_FALSE(map.has_hd());
}

TEST_CASE("Far map off the frame: no picture at first, then it arrives",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const auto tiles = test_city();
  const auto pal = test_palette(101); // no other test paints this one
  views::hd::PictureJob job;
  CHECK(views::hd::far_map_picture_soon(job, tiles, columns, rows, tile_px,
                                        pal) == nullptr);
  CHECK(job.started());

  const auto picture = wait_for(job);
  REQUIRE(picture != nullptr);
  // The picture the frame would have painted, now in the cache: asking
  // again gives it at once, without a job.
  CHECK(views::hd::far_map_picture(tiles, columns, rows, tile_px, pal) ==
        picture);
  CHECK(views::hd::far_map_picture_soon(job, tiles, columns, rows, tile_px,
                                        pal) == picture);
  CHECK_FALSE(job.started());

  // Attached as the view does it: same pixels, art, the whole map.
  Surface map = indexed_far_map(tiles);
  const auto before = pixels_of(map);
  REQUIRE(views::hd::attach_far_map_picture(map, picture));
  CHECK(pixels_of(map) == before);
  CHECK(all_art(map));
  CHECK(map.hd() == picture.get());
  Surface other(10, 10);
  CHECK_FALSE(views::hd::attach_far_map_picture(other, picture));
}

TEST_CASE("Far map off the frame: a stale build is discarded", "[hd][visual]") {
  ScopedHdScale scale(2);
  const int t = tile_px * 2; // texels per tile
  const auto tiles = test_city();
  const auto autumn = test_palette(60);
  const auto winter = test_palette(120);
  views::hd::PictureJob job;
  CHECK(views::hd::far_map_picture_soon(job, tiles, columns, rows, tile_px,
                                        autumn) == nullptr);
  // The season changes before the autumn picture is there.
  CHECK(views::hd::far_map_picture_soon(job, tiles, columns, rows, tile_px,
                                        winter) == nullptr);
  const auto picture = wait_for(job);
  REQUIRE(picture != nullptr);
  CHECK(picture ==
        views::hd::far_map_picture(tiles, columns, rows, tile_px, winter));
  CHECK(picture !=
        views::hd::far_map_picture(tiles, columns, rows, tile_px, autumn));
  // Winter's land colour (with its faint grain), not autumn's.
  const uint32_t land = picture->at(140 * t + 1, 20 * t + 1);
  CHECK(std::abs(static_cast<int>(gfx::red(land)) - winter[ground].r) <= 12);
  // And nothing else comes later.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(job.take() == nullptr);
  CHECK_FALSE(job.started());
}

TEST_CASE("Medium map: at hd_scale 1 nothing changes", "[hd][visual]") {
  ScopedHdScale scale(1);
  const Surface tiles = medium_tiles();
  const Surface landsym(512, 512);
  const std::vector<uint8_t> types(12, 5);
  CHECK_FALSE(views::hd::medium_map_input(tiles, landsym, types, 4, 3,
                                          gfx::PaletteHandle{0})
                  .has_value());
  CHECK(views::hd::medium_tile_sheet(nullptr, 512, 512, 128, 128, 1) ==
        nullptr);
}

TEST_CASE("Medium map: tiles from HD landsym, area-averaged, same pixels",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  constexpr int cols = 10;
  constexpr int map_rows = 6;
  const Surface tiles = medium_tiles();
  const Surface landsym = checker_landsym(k);
  std::vector<uint8_t> types(cols * map_rows);
  for (std::size_t i = 0; i < types.size(); ++i) {
    types[i] = static_cast<uint8_t>(i * 7 % 256);
  }
  types[0] = 3; // the checker tile

  // The indexed map as CityView always drew it.
  Surface reference(cols * 8, map_rows * 8);
  for (int i = 0; i < cols * map_rows; ++i) {
    reference.blit(tiles,
                   landsym_source_rect(types[static_cast<std::size_t>(i)]),
                   ViewPoint{(i % cols) * 8, (i / cols) * 8});
  }
  Surface drawn(cols * 8, map_rows * 8);
  CHECK(views::hd::draw_medium_map(drawn, tiles, types, cols));
  CHECK(pixels_of(drawn) == pixels_of(reference));

  const auto input = views::hd::medium_map_input(
      tiles, landsym, types, cols, map_rows, gfx::PaletteHandle{0});
  REQUIRE(input.has_value());
  const auto map = views::hd::build_medium_map(*input);
  REQUIRE(map != nullptr);
  CHECK(pixels_of(*map) == pixels_of(reference));
  REQUIRE(map->has_hd());
  CHECK(all_art(*map));
  CHECK(map->hd_list()->cmds().size() == types.size()); // a quad per tile

  const HdImage* hd = map->hd();
  REQUIRE(hd != nullptr);
  for (int i = 0; i < cols * map_rows; ++i) {
    const uint8_t type = types[static_cast<std::size_t>(i)];
    if (type == 3) {
      continue; // the checker, below
    }
    const int x = (i % cols) * 8 * k;
    const int y = (i / cols) * 8 * k;
    CHECK(hd->at(x + 3, y + 5) == rgba(tile_colour(type)));
  }
  // The checker averages to mid grey: each texel is a 4x4 block of HD
  // landsym, half black, half white.
  const uint32_t grey = hd->at(3, 3);
  CHECK(gfx::red(grey) >= 127);
  CHECK(gfx::red(grey) <= 128);
  CHECK(gfx::alpha(grey) == 255);

  // The sheet is cached: built once per landsym picture and scale.
  const auto& leaf = landsym.hd_list()->cmds().front().img;
  CHECK(views::hd::medium_tile_sheet(leaf, 512, 512, 128, 128, k) ==
        views::hd::medium_tile_sheet(leaf, 512, 512, 128, 128, k));
}

TEST_CASE("Medium map: built off the frame", "[hd][visual]") {
  ScopedHdScale scale(2);
  const Surface tiles = medium_tiles();
  const Surface landsym = checker_landsym(2);
  const std::vector<uint8_t> types(8 * 4, 17);
  auto input = views::hd::medium_map_input(tiles, landsym, types, 8, 4,
                                           gfx::PaletteHandle{0});
  REQUIRE(input.has_value());
  const auto sync = views::hd::build_medium_map(*input);
  REQUIRE(sync != nullptr);
  views::hd::SurfaceJob job;
  views::hd::start_medium_map(job, std::move(*input));
  CHECK(job.started());
  const auto map = wait_for(job);
  REQUIRE(map != nullptr);
  CHECK(pixels_of(*map) == pixels_of(*sync));
  REQUIRE(map->hd() != nullptr);
  CHECK(map->hd()->pixels == sync->hd()->pixels);
  CHECK(all_art(*map));
}

TEST_CASE("Water: at hd_scale 1 nothing", "[hd][visual]") {
  ScopedHdScale scale(1);
  const Surface landsym = water_landsym();
  CHECK_FALSE(views::hd::water_cycle_input(landsym, water_palette(),
                                           water_indices, water_cycles)
                  .has_value());
}

TEST_CASE("Water: each step's picture has that step's colours, on water only",
          "[hd][visual]") {
  ScopedHdScale scale(2);
  const int k = 2;
  const auto pal = water_palette();
  Surface landsym = water_landsym();
  landsym.attach_hd(palette_picture(landsym, pal, k));
  // Without HD landsym (no pack) there is nothing to recolour.
  CHECK_FALSE(views::hd::water_cycle_input(water_landsym(), pal, water_indices,
                                           water_cycles)
                  .has_value());

  const auto input =
      views::hd::water_cycle_input(landsym, pal, water_indices, water_cycles);
  REQUIRE(input.has_value());
  const auto frames = views::hd::build_water_frames(*input);
  REQUIRE(frames != nullptr);
  CHECK(frames->water_tiles() == 2);
  CHECK(frames->has_water(5));
  CHECK(frames->has_water(33));
  CHECK_FALSE(frames->has_water(0));
  CHECK_FALSE(frames->has_step(0)); // the art as it is
  for (int step = 1; step < 4; ++step) {
    REQUIRE(frames->has_step(step));
    // Water texels only: 16x32 + 1 water pixels, k*k texels each.
    const auto& img = frames->pictures()[static_cast<std::size_t>(step)];
    int seen = 0;
    for (uint32_t texel : img->pixels) {
      seen += gfx::alpha(texel) != 0 ? 1 : 0;
    }
    CHECK(seen == (16 * 32 + 1) * k * k);
  }

  // Drawn as the view draws it: the tiles, then the water over them.
  const ViewRect five = landsym_source_rect(5, BASE_MAP_ZOOM_MULTIPLIER);
  const ViewRect t33 = landsym_source_rect(33, BASE_MAP_ZOOM_MULTIPLIER);
  for (int step = 1; step < 4; ++step) {
    Surface screen(80, 40, std::nullopt, 240);
    screen.blit(landsym, five, ViewPoint{4, 2});
    screen.blit(landsym, t33, ViewPoint{40, 2});
    const auto before = pixels_of(screen);
    CHECK(frames->overlay(screen, 5, ViewPoint{4, 2}, step));
    CHECK(frames->overlay(screen, 33, ViewPoint{40, 2}, step));
    CHECK_FALSE(frames->overlay(screen, 0, ViewPoint{0, 0}, step));
    CHECK(pixels_of(screen) == before);
    CHECK(all_art(screen));
    CHECK(screen.hd_list()->cmds().size() == 4); // 2 tiles + 2 quads

    const HdImage* hd = screen.hd();
    REQUIRE(hd != nullptr);
    int wrong = 0;
    for (const auto& [tile, at] :
         {std::pair{five, ViewPoint{4, 2}}, std::pair{t33, ViewPoint{40, 2}}}) {
      for (int y = 0; y < tile.h; ++y) {
        for (int x = 0; x < tile.w; ++x) {
          const uint8_t index = landsym.get(ViewPoint{tile.x + x, tile.y + y});
          Color want = pal[index];
          if (index >= 140 && index <= 143) {
            want = pal[water_cycles[static_cast<std::size_t>(step)]
                                   [static_cast<std::size_t>(index - 140)]];
          }
          const uint32_t texel = hd->at((at.x + x) * k + 1, (at.y + y) * k);
          wrong += texel != rgba(want) ? 1 : 0;
        }
      }
    }
    CHECK(wrong == 0); // water in the step's colours, land as it was
  }
  // Step 0 draws nothing.
  Surface screen(40, 40);
  screen.blit(landsym, five, ViewPoint{0, 0});
  CHECK_FALSE(frames->overlay(screen, 5, ViewPoint{0, 0}, 0));
}

TEST_CASE("Water: built off the frame", "[hd][visual]") {
  ScopedHdScale scale(2);
  auto pal = water_palette();
  pal[140] = {21, 41, 161}; // its own pictures, not the cached ones
  Surface landsym = water_landsym();
  landsym.attach_hd(palette_picture(landsym, pal, 2));
  auto input =
      views::hd::water_cycle_input(landsym, pal, water_indices, water_cycles);
  REQUIRE(input.has_value());
  views::hd::WaterJob job;
  views::hd::start_water_frames(job, std::move(*input));
  const auto frames = wait_for(job);
  REQUIRE(frames != nullptr);
  CHECK(frames->water_tiles() == 2);
  CHECK(frames->has_step(2));
}

// Not run by default: `test_plegacy "[bench]"` (a Release build) prints
// what the 4K (k = 6) pictures cost.
TEST_CASE("HD city maps at 4K: build time, memory and per-frame cost",
          "[.][bench][hd]") {
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  ScopedHdScale scale(6);
  const int k = 6;

  // A full landsym: land everywhere, water in every fifth tile type (a
  // third of those tiles' pixels), with some texture in HD.
  auto pal = water_palette();
  Surface landsym(512, 512, std::nullopt, 28);
  for (int y = 0; y < 512; ++y) {
    for (int x = 0; x < 512; ++x) {
      const int type = (x / 32) * 16 + y / 32;
      const bool wet = type % 5 == 0 && (x + y) % 32 < 10;
      landsym.put(ViewPoint{x, y},
                  static_cast<uint8_t>(wet ? 140 + x % 4 : 28 + (x + y) % 3));
    }
  }
  {
    auto img = palette_picture(landsym, pal, k);
    for (std::size_t i = 0; i < img->pixels.size(); ++i) {
      img->pixels[i] ^= (static_cast<uint32_t>(i * 2654435761u) >> 29) << 8;
    }
    landsym.attach_hd(img);
  }

  // M4: the water pictures.
  auto t0 = clock::now();
  const auto input =
      views::hd::water_cycle_input(landsym, pal, water_indices, water_cycles);
  auto t1 = clock::now();
  REQUIRE(input.has_value());
  const auto frames = views::hd::build_water_frames(*input);
  auto t2 = clock::now();
  REQUIRE(frames != nullptr);
  int pictures = 0;
  for (const auto& img : frames->pictures()) {
    pictures += img ? 1 : 0;
  }
  std::printf("[bench] water: input copy %.2f ms (game thread), build %.1f ms "
              "(worker), %d water tiles, %.1f MB in %d pictures\n",
              ms(t1 - t0), ms(t2 - t1), frames->water_tiles(),
              static_cast<double>(frames->bytes()) / (1 << 20), pictures);

  // M4: per frame at CLOSE zoom, a 600x330 panel of 32-pixel tiles.
  constexpr int frames_run = 200;
  double blit_ms = 0;
  double overlay_ms = 0;
  int overlays = 0;
  for (int f = 0; f < frames_run; ++f) {
    Surface screen(640, 400, std::nullopt, 240);
    const auto a = clock::now();
    std::vector<std::pair<uint8_t, ViewPoint>> wet;
    for (int ty = 0; ty < 13; ++ty) {
      for (int tx = 0; tx < 20; ++tx) {
        const auto type = static_cast<uint8_t>((tx * 3 + ty * 5 + f) % 256);
        const ViewPoint at{tx * 32 - 7, ty * 32 - 5};
        screen.blit(landsym,
                    landsym_source_rect(type, BASE_MAP_ZOOM_MULTIPLIER), at);
        if (frames->has_water(type)) {
          wet.emplace_back(type, at);
        }
      }
    }
    const auto b = clock::now();
    for (const auto& [type, at] : wet) {
      overlays += frames->overlay(screen, type, at, 1 + f % 3) ? 1 : 0;
    }
    const auto c = clock::now();
    blit_ms += ms(b - a);
    overlay_ms += ms(c - b);
  }
  std::printf("[bench] water per frame: tiles %.3f ms, water quads %.3f ms "
              "(%.1f quads)\n",
              blit_ms / frames_run, overlay_ms / frames_run,
              static_cast<double>(overlays) / frames_run);

  // M6: the sheet and the whole medium map (worker).
  const Surface tiles = medium_tiles();
  std::vector<uint8_t> types(MAP_TILES_PER_ROW * MAP_TILES_PER_COLUMN);
  for (std::size_t i = 0; i < types.size(); ++i) {
    types[i] = static_cast<uint8_t>((i * 31) % 256);
  }
  t0 = clock::now();
  const auto medium_in =
      views::hd::medium_map_input(tiles, landsym, types, MAP_TILES_PER_ROW,
                                  MAP_TILES_PER_COLUMN, gfx::PaletteHandle{0});
  t1 = clock::now();
  REQUIRE(medium_in.has_value());
  const auto sheet = views::hd::medium_tile_sheet(
      medium_in->landsym_hd, 512, 512, tiles.width(), tiles.height(), k);
  t2 = clock::now();
  const auto medium = views::hd::build_medium_map(*medium_in);
  const auto t3 = clock::now();
  REQUIRE(medium != nullptr);
  std::printf(
      "[bench] medium: input copy %.2f ms (game thread), sheet %.1f ms, "
      "map %.1f ms (worker), sheet %.1f MB\n",
      ms(t1 - t0), ms(t2 - t1), ms(t3 - t2),
      static_cast<double>(sheet->pixels.size() * 4) / (1 << 20));
  double medium_frame = 0;
  for (int f = 0; f < 50; ++f) {
    Surface screen(640, 400);
    const auto a = clock::now();
    screen.blit(*medium, ViewRect{(f * 13) % 600, (f * 7) % 600, 600, 330},
                ViewPoint{20, 40});
    medium_frame += ms(clock::now() - a);
  }
  std::printf("[bench] medium per frame: map blit %.3f ms\n",
              medium_frame / 50);

  // H4: the far map, asked for on the frame and painted on a worker.
  std::vector<FarMapTile> far(static_cast<std::size_t>(columns) * rows);
  for (std::size_t i = 0; i < far.size(); ++i) {
    far[i] = {static_cast<uint8_t>(i % 7 == 0 ? water : ground),
              i % 7 == 0 ? MapTileClass::Water : MapTileClass::Ground};
  }
  views::hd::PictureJob job;
  t0 = clock::now();
  const auto none = views::hd::far_map_picture_soon(job, far, columns, rows,
                                                    tile_px, test_palette(201));
  t1 = clock::now();
  CHECK(none == nullptr);
  const auto far_picture = wait_for(job);
  t2 = clock::now();
  REQUIRE(far_picture != nullptr);
  std::printf("[bench] far map: on the frame %.3f ms, ready after %.1f ms\n",
              ms(t1 - t0), ms(t2 - t1));
}
