/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "TestSupport.h"
#include "engine/gfx/Png.h"
#include "resources/BobLoader.h"
#include "resources/HdPack.h"
#include "resources/PaletteLoader.h"
#include "resources/VgaLoader.h"
#include "resources/util/FileUtils.h"

using namespace gfx;
using resources::HdPack;
using test_support::ScopedHdScale;
using test_support::TempDir;

namespace {
// Leaves the pack switched off for the next test.
struct PackReset {
  ~PackReset() { HdPack::instance().configure({}); }
};
} // namespace

TEST_CASE("pack keys follow the export layout", "[hd][pack]") {
  CHECK(HdPack::normalize_key("GFX\\Back1.VGA") == "gfx/back1");
  CHECK(HdPack::normalize_key("bob/moebel.bob") == "bob/moebel");
  CHECK(HdPack::normalize_key("./z/zb3") == "z/zb3");
  CHECK(HdPack::vga_relative_path("gfx/euromap.vga") == "gfx/euromap.png");
  CHECK(HdPack::bob_frame_relative_path("bob/walk3.bob", 7) ==
        "bob/walk3/0007.png");
#ifndef _WIN32
  CHECK(HdPack::normalize_key("/usr/share/plegacy/data/game_location") ==
        "plegacy/game_location");
#endif
}

TEST_CASE("PNG round trip keeps every texel", "[hd][png]") {
  TempDir dir;
  HdImage img(5, 3);
  for (int i = 0; i < 15; ++i) {
    img.pixels[static_cast<std::size_t>(i)] =
        pack_rgba(static_cast<uint32_t>(i * 17), 255 - static_cast<uint32_t>(i),
                  static_cast<uint32_t>(i * 3), i == 4 ? 0u : 200u);
  }
  REQUIRE(save_png(img, dir.path() / "sub" / "x.png"));
  auto back = load_png(dir.path() / "sub" / "x.png");
  REQUIRE(back);
  REQUIRE(back->width == 5);
  REQUIRE(back->height == 3);
  for (int i = 0; i < 15; ++i) {
    if (i == 4) {
      CHECK(alpha(back->pixels[4]) == 0);
    }
    else {
      CHECK(back->pixels[static_cast<std::size_t>(i)] ==
            img.pixels[static_cast<std::size_t>(i)]);
    }
  }
}

TEST_CASE("pack is off without a folder or at scale 1", "[hd][pack]") {
  PackReset reset;
  TempDir dir;
  {
    ScopedHdScale scale(1);
    HdPack::instance().configure(dir.path());
    CHECK_FALSE(HdPack::instance().enabled());
  }
  ScopedHdScale scale(4);
  HdPack::instance().configure(dir.path() / "does-not-exist");
  CHECK_FALSE(HdPack::instance().enabled());
  CHECK(HdPack::instance().vga("gfx/back1", Size{384, 352}) == nullptr);
}

TEST_CASE("pack resamples to the engine size and rejects wrong shapes",
          "[hd][pack]") {
  PackReset reset;
  TempDir dir;
  ScopedHdScale scale(3);
  // 10x10 original -> 20x20 engine surface -> 60x60 at scale 3.
  REQUIRE(save_png(HdImage(120, 120, pack_rgba(10, 20, 30, 255)),
                   dir.path() / "gfx" / "square.png"));
  REQUIRE(save_png(HdImage(200, 100, pack_rgba(1, 2, 3, 255)),
                   dir.path() / "gfx" / "wide.png"));
  HdPack::instance().configure(dir.path());
  REQUIRE(HdPack::instance().enabled());

  auto ok = HdPack::instance().vga("gfx/square", Size{20, 20});
  REQUIRE(ok);
  CHECK(ok->width == 60);
  CHECK(ok->height == 60);
  CHECK(ok->at(30, 30) == pack_rgba(10, 20, 30, 255));
  // Cached: same object the second time.
  CHECK(HdPack::instance().vga("gfx/square", Size{20, 20}) == ok);

  CHECK(HdPack::instance().vga("gfx/wide", Size{20, 20}) == nullptr);
  CHECK(HdPack::instance().vga("gfx/missing", Size{20, 20}) == nullptr);
  CHECK(HdPack::instance().stats().loaded == 1);
  CHECK(HdPack::instance().stats().rejected == 1);
  CHECK(HdPack::instance().stats().missing == 1);
}

TEST_CASE("real VGA and BOB loads pick up pack art", "[hd][pack][game]") {
  const auto game = test_support::game_path();
  if (!game) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  PackReset reset;
  TempDir dir;
  ScopedHdScale scale(2);
  fileutils::game_data_path(game->string());
  resources::load_palette_bank();

  // BACK1.VGA is 192x176; BOB/MOEBEL.BOB frame 0 is 21x23.
  const uint32_t marker = pack_rgba(250, 10, 200, 255);
  REQUIRE(save_png(HdImage(192 * 4, 176 * 4, marker),
                   dir.path() / "gfx" / "back1.png"));
  REQUIRE(save_png(HdImage(21 * 8, 23 * 8, marker),
                   dir.path() / "bob" / "moebel" / "0000.png"));
  HdPack::instance().configure(dir.path());

  VgaLoader vga;
  const Surface back = vga.load("gfx/back1");
  REQUIRE(back.has_hd());
  CHECK(back.width() == 384);
  CHECK(back.hd()->width == 384 * 2);
  CHECK(back.hd()->at(100, 100) == marker);

  const Surface back2 = vga.load("gfx/back2");
  CHECK_FALSE(back2.has_hd());

  BobLoader bobs;
  auto moebel = bobs.load("bob/moebel.bob");
  const Surface frame0 = moebel->frame(0);
  REQUIRE(frame0.has_hd());
  CHECK(frame0.hd()->width == 21 * 2 * 2);
  CHECK_FALSE(moebel->frame(1).has_hd());
}

TEST_CASE("warm list round trip and background prefetch", "[hd][pack]") {
  ScopedHdScale scale(2);
  PackReset reset;
  TempDir dir;
  const auto root = dir.path() / "pack";
  // Three pictures of 8x4 engine pixels (4x2 originals doubled), 16x8 texels.
  for (const char* name : {"gfx/a.png", "gfx/b.png", "z/c.png"}) {
    REQUIRE(save_png(HdImage(16, 8, pack_rgba(10, 20, 30, 255)), root / name));
  }
  auto& pack = HdPack::instance();
  pack.configure(root);

  // A session uses two of them; the list keeps the order of first use.
  const Size engine{8, 4};
  REQUIRE(pack.vga("gfx/b", engine));
  REQUIRE(pack.vga("gfx/a", engine));
  CHECK_FALSE(pack.vga("gfx/missing", engine));
  const auto used = pack.used();
  REQUIRE(used.size() == 2);
  CHECK(used[0].relative == "gfx/b.png");
  REQUIRE(HdPack::save_warm_list(dir.path() / "warm.txt", used));

  // Unsafe lines are skipped.
  {
    std::ofstream extra(dir.path() / "warm.txt", std::ios::app);
    extra << "8 4 ../outside.png\n0 4 gfx/a.png\nnot a line\n8 4 z/c.png\n";
  }
  const auto list = HdPack::load_warm_list(dir.path() / "warm.txt");
  REQUIRE(list.size() == 3);
  CHECK(list[2].relative == "z/c.png");

  // Next session: everything on the list is decoded in the background, and
  // later loads come straight from the cache.
  pack.configure(root);
  pack.prefetch(list, 1 << 30);
  pack.wait_for_prefetch();
  CHECK(pack.stats().loaded == 3);
  const auto a = pack.vga("gfx/a", engine);
  REQUIRE(a);
  CHECK(a->at(0, 0) == pack_rgba(10, 20, 30, 255));
  CHECK(pack.stats().loaded == 3); // no second decode

  // The budget stops the prefetch early.
  pack.configure(root);
  pack.prefetch(list, 1);
  pack.wait_for_prefetch();
  CHECK(pack.stats().loaded == 1);
}

TEST_CASE("a load waits for a prefetch already decoding the file",
          "[hd][pack]") {
  ScopedHdScale scale(2);
  PackReset reset;
  TempDir dir;
  const auto root = dir.path() / "pack";
  // A big picture, so the prefetch is still busy when the load comes in.
  REQUIRE(save_png(HdImage(2048, 1024, pack_rgba(1, 2, 3, 255)),
                   root / "gfx/big.png"));
  auto& pack = HdPack::instance();
  pack.configure(root);
  pack.prefetch({{"gfx/big.png", Size{1024, 512}}}, 1 << 30);
  const auto img = pack.vga("gfx/big", Size{1024, 512});
  REQUIRE(img);
  pack.wait_for_prefetch();
  CHECK(pack.stats().loaded == 1); // decoded once, by whoever came first
  CHECK(pack.vga("gfx/big", Size{1024, 512}) == img);
}
