/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The warm list's screen tags: older three-field lists still load, the
// screen that used a picture is recorded, and the screen being opened gets
// its pictures decoded first (HdPack::screen front-loads the queue).

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Png.h"
#include "resources/HdPack.h"

namespace fs = std::filesystem;
using resources::HdPack;
using test_support::ScopedHdScale;

namespace {

std::vector<std::string> names(const std::vector<HdPack::WarmEntry>& entries) {
  std::vector<std::string> out;
  for (const auto& e : entries) {
    out.push_back(e.relative);
  }
  return out;
}

// Puts the pack back the way the other tests expect it.
struct ScopedPack {
  explicit ScopedPack(const fs::path& root) {
    HdPack::instance().configure(root);
  }
  ~ScopedPack() { HdPack::instance().configure({}); }
  ScopedPack(const ScopedPack&) = delete;
  ScopedPack& operator=(const ScopedPack&) = delete;
};

} // namespace

TEST_CASE("warm lists with and without screen tags", "[hd][warmup]") {
  test_support::TempDir dir;
  const fs::path file = dir.path() / "hd_warm.txt";
  std::ofstream(file) << "# an older list\n"
                         "640 400 gfx/bo.png\n"
                         "56 44 bob/moebel/0116.png restaurant\n"
                         "0 4 gfx/bad.png\n"
                         "10 10 ../escape.png\n";
  const auto loaded = HdPack::load_warm_list(file);
  REQUIRE(loaded.size() == 2);
  CHECK(loaded[0].relative == "gfx/bo.png");
  CHECK(loaded[0].screen.empty());
  CHECK(loaded[1].screen == "restaurant");

  std::vector<HdPack::WarmEntry> tagged = loaded;
  tagged.push_back({"gfx/euromap.png", Size{640, 400}, "euro map"});
  REQUIRE(HdPack::save_warm_list(file, tagged));
  const auto back = HdPack::load_warm_list(file);
  REQUIRE(back.size() == 3);
  CHECK(back[0].screen.empty()); // still a plain three-field line
  CHECK(back[1].screen == "restaurant");
  CHECK(back[2].screen == "euro_map"); // one word
  std::ifstream in(file);
  std::string line;
  std::getline(in, line); // the header
  std::getline(in, line);
  CHECK(line == "640 400 gfx/bo.png");
}

TEST_CASE("front-loading keeps both groups in their order", "[hd][warmup]") {
  std::deque<HdPack::WarmEntry> q{{"a1", {1, 1}, "a"},
                                  {"b1", {1, 1}, "b"},
                                  {"x", {1, 1}, ""},
                                  {"a2", {1, 1}, "a"},
                                  {"b2", {1, 1}, "b"}};
  HdPack::front_load(q, "b");
  std::vector<std::string> order;
  for (const auto& e : q) {
    order.push_back(e.relative);
  }
  CHECK(order == std::vector<std::string>{"b1", "b2", "a1", "x", "a2"});
  HdPack::front_load(q, ""); // no screen: as it was
  CHECK(q.front().relative == "b1");
}

TEST_CASE("the screen being opened is decoded first, and named in the list",
          "[hd][warmup]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const fs::path pack_root = dir.path() / "pack";
  for (const char* n : {"a1", "a2", "b1", "b2", "c", "d"}) {
    REQUIRE(gfx::save_png(gfx::HdImage(20, 20, gfx::pack_rgba(10, 20, 30, 255)),
                          pack_root / "gfx" / (std::string(n) + ".png")));
  }
  ScopedPack scoped(pack_root);
  HdPack& pack = HdPack::instance();
  REQUIRE(pack.enabled());

  // The screen is already known when the list is queued: its pictures go
  // first.
  pack.screen("b");
  pack.prefetch({{"gfx/a1.png", {10, 10}, "a"},
                 {"gfx/b1.png", {10, 10}, "b"},
                 {"gfx/a2.png", {10, 10}, "a"},
                 {"gfx/b2.png", {10, 10}, "b"}},
                std::size_t{1} << 30);
  pack.wait_for_prefetch();
  CHECK(pack.queued().empty());
  CHECK(names(pack.used()) ==
        std::vector<std::string>{"gfx/b1.png", "gfx/b2.png", "gfx/a1.png",
                                 "gfx/a2.png"});
  (void)pack.take_prefetched();

  // A picture a screen loads itself is recorded with that screen...
  pack.screen("menu3");
  REQUIRE(pack.vga("gfx/c", Size{10, 10}));
  CHECK(pack.used().back().relative == "gfx/c.png");
  CHECK(pack.used().back().screen == "menu3");

  // ...and one decoded ahead without a name learns it on first use.
  pack.prefetch({{"gfx/d.png", {10, 10}, ""}}, std::size_t{1} << 30);
  pack.wait_for_prefetch();
  (void)pack.take_prefetched();
  pack.screen("cityview");
  REQUIRE(pack.vga("gfx/d", Size{10, 10}));
  CHECK(pack.used().back().relative == "gfx/d.png");
  CHECK(pack.used().back().screen == "cityview");
  // Pictures already named keep their screen.
  REQUIRE(pack.vga("gfx/b1", Size{10, 10}));
  CHECK(pack.used().front().screen == "b");
}
