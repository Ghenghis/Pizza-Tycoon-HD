/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The start-up warm list (fx::session_warm_list): a first session has no
// warm list of its own, so the pack's list and its biggest pictures are
// decoded in the background, not on the first frame of each screen.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/fx/PackWarmup.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Png.h"
#include "resources/HdPack.h"

namespace fs = std::filesystem;
using test_support::ScopedHdScale;

namespace {

void write_vga_header(const fs::path& file, int w, int h) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  const unsigned char header[] = {static_cast<unsigned char>(w & 0xFF),
                                  static_cast<unsigned char>(w >> 8),
                                  static_cast<unsigned char>(h & 0xFF),
                                  static_cast<unsigned char>(h >> 8),
                                  0,
                                  0,
                                  0,
                                  0};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
}

void write_png(const fs::path& file, int w, int h) {
  REQUIRE(
      gfx::save_png(gfx::HdImage(w, h, gfx::pack_rgba(9, 9, 9, 255)), file));
}

void write_text(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file) << text;
}

} // namespace

TEST_CASE("the pack's pictures are found with their engine sizes",
          "[hd][warmup]") {
  test_support::TempDir dir;
  const fs::path pack = dir.path() / "pack";
  const fs::path game = dir.path() / "game";
  write_png(pack / "gfx" / "big.png", 64, 40);    // 320x200 original
  write_png(pack / "gfx" / "small.png", 38, 35);  // 192x176 original
  write_png(pack / "gfx" / "odd.png", 50, 50);    // the wrong shape
  write_png(pack / "gfx" / "orphan.png", 64, 40); // no original
  write_vga_header(game / "GFX" / "BIG.VGA", 320, 200);
  write_vga_header(game / "GFX" / "Small.vga", 192, 176);
  write_vga_header(game / "GFX" / "ODD.VGA", 320, 200);

  const auto found = fx::scan_pack_pictures(pack, game);
  REQUIRE(found.size() == 2);
  CHECK(found[0].relative == "gfx/big.png"); // biggest first
  CHECK(found[0].engine_size.w == 640);
  CHECK(found[0].engine_size.h == 400);
  CHECK(found[1].relative == "gfx/small.png");
  CHECK(found[1].engine_size.w == 384);
  CHECK(found[1].engine_size.h == 352);
  CHECK(fx::scan_pack_pictures(pack, {}).empty());
  CHECK(fx::scan_pack_pictures(dir.path() / "none", game).empty());
}

TEST_CASE("the session's warm list: the player's, the pack's, then the scan",
          "[hd][warmup]") {
  test_support::TempDir dir;
  const fs::path pack = dir.path() / "pack";
  const fs::path game = dir.path() / "game";
  write_png(pack / "gfx" / "big.png", 64, 40);
  write_png(pack / "gfx" / "small.png", 38, 35);
  write_vga_header(game / "gfx" / "big.vga", 320, 200);
  write_vga_header(game / "gfx" / "small.vga", 192, 176);
  const fs::path user = dir.path() / "hd_warm.txt";
  write_text(user, "# a comment\n10 10 bob/x/0001.png\n640 400 gfx/big.png\n");
  write_text(pack / "hd_warm.txt",
             "20 20 bob/y/0002.png\n10 10 bob/x/0001.png\n");

  const auto list = fx::session_warm_list(user, pack, game, 6);
  std::vector<std::string> names;
  for (const auto& e : list) {
    names.push_back(e.relative);
  }
  CHECK(names == std::vector<std::string>{"bob/x/0001.png", "gfx/big.png",
                                          "bob/y/0002.png", "gfx/small.png"});

  // The scan stays within its budget; the lists are never cut.
  const auto tight = fx::session_warm_list(user, pack, game, 6, 1);
  CHECK(tight.size() == 3);
  // No pack, no game folder: just the player's list.
  CHECK(fx::session_warm_list(user, {}, game, 6).size() == 2);
  CHECK(fx::session_warm_list(user, pack, {}, 6).size() == 3);
  // A first session: no list of its own at all.
  CHECK(
      fx::session_warm_list(dir.path() / "missing.txt", pack, game, 6).size() ==
      4);
}

// Run with: test_plegacy "[.bench][warmup]", with PLEGACY_TEST_GAME_PATH
// and PLEGACY_TEST_HD_PACK (a pack made for --hd-scale 6, e.g. the gate's).
TEST_CASE("a first open of each screen, cold and after the background "
          "decode",
          "[.bench][warmup]") {
  const auto game = test_support::game_path();
  const char* pack_env = std::getenv("PLEGACY_TEST_HD_PACK");
  if (!game || !pack_env || !*pack_env) {
    SKIP("Needs PLEGACY_TEST_GAME_PATH and PLEGACY_TEST_HD_PACK");
  }
  const fs::path pack_root(pack_env);
  ScopedHdScale scale(6);
  auto& pack = resources::HdPack::instance();
  const auto pictures = fx::scan_pack_pictures(pack_root, *game);
  REQUIRE_FALSE(pictures.empty());
  using clock = std::chrono::steady_clock;
  auto open_all = [&] {
    double worst = 0;
    double total = 0;
    for (const auto& e : pictures) {
      const auto t0 = clock::now();
      const std::string key = e.relative.substr(0, e.relative.size() - 4);
      (void)pack.vga(key, e.engine_size);
      const double ms =
          std::chrono::duration<double, std::milli>(clock::now() - t0).count();
      worst = std::max(worst, ms);
      total += ms;
    }
    return std::pair{worst, total};
  };

  pack.configure(pack_root);
  const auto [cold_worst, cold_total] = open_all();

  pack.configure(pack_root); // a fresh session: nothing decoded
  const auto t0 = clock::now();
  pack.prefetch(
      fx::session_warm_list({}, pack_root, *game, 6, std::size_t{4} << 30),
      std::size_t{4} << 30);
  pack.wait_for_prefetch();
  const double background_ms =
      std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  const auto [warm_worst, warm_total] = open_all();
  (void)pack.take_prefetched();
  pack.configure({});

  std::cout << pictures.size() << " pack pictures at hd_scale 6: first open "
            << "on the frame worst " << cold_worst << " ms, all " << cold_total
            << " ms; after the background decode (" << background_ms
            << " ms off the frame) worst " << warm_worst << " ms, all "
            << warm_total << " ms\n";
  CHECK(warm_worst < cold_worst);
}
