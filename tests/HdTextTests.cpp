/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// HD text: lines drawn with the shipped HD typefaces at full resolution, in
// the place and alignment the original bitmap font gives them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "TestSupport.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Surface.h"
#include "resources/Font.h"
#include "resources/FontTypes.h"
#include "resources/HdTypeface.h"
#include "resources/PaletteLoader.h"

using gfx::HdImage;
using gfx::Surface;
using resources::HdTypeface;
using resources::HdTypefaces;

namespace {

// The shipped data/fonts folder, from the build or the source tree.
std::filesystem::path fonts_dir() {
  for (const char* p : {"data/fonts", "../data/fonts", "../../data/fonts"}) {
    if (std::filesystem::exists(std::filesystem::path(p) / "hd_fonts.json")) {
      return p;
    }
  }
  return {};
}

std::shared_ptr<const HdTypeface> chakra() {
  const auto dir = fonts_dir();
  REQUIRE_FALSE(dir.empty());
  HdTypeface::Settings s;
  s.file = "ChakraPetch-SemiBold.ttf";
  auto t = HdTypeface::load(dir / s.file, s);
  REQUIRE(t);
  return t;
}

// A tiny bitmap font: every glyph a solid 3x5 block in a 5-row line, two
// engine pixels per original pixel (like the game's), advance 4.
std::unique_ptr<Font> block_font() {
  resources::load_fallback_palette();
  Surface atlas(6 * 96, 10, gfx::PaletteHandle{0}, 0);
  std::unordered_map<uint32_t, Glyph> glyphs;
  for (uint32_t c = 32; c < 128; ++c) {
    const auto x = static_cast<uint16_t>((c - 32) * 6);
    if (c != ' ') {
      atlas.fill_rect(ViewRect{x, 0, 6, 10}, gfx::ColorIndex{1});
    }
    glyphs.emplace(c, Glyph{.start_position_x = x,
                            .start_position_y = 0,
                            .width = static_cast<uint16_t>(c == ' ' ? 0 : 6),
                            .height = 10,
                            .advance = 8,
                            .character = c});
  }
  atlas.disable_palette_translation();
  FontData data{.surface = std::move(atlas),
                .glyphs = std::move(glyphs),
                .line_height = 10};
  return std::make_unique<Font>(FontFace::SUPERSMA, std::move(data));
}

int count_colours(const HdImage& img) {
  std::set<uint32_t> seen;
  for (uint32_t p : img.pixels) {
    seen.insert(p);
  }
  return static_cast<int>(seen.size());
}

} // namespace

TEST_CASE("HD typeface lays out a line at the cap height asked for") {
  auto t = chakra();
  const float cap = 120;
  const auto line = t->line(U"HHH", cap, 0, 10000);
  REQUIRE_FALSE(line.empty());
  // Rows with ink of the H: from the baseline up by the cap height.
  int top = line.height;
  int bottom = -1;
  for (int y = 0; y < line.height; ++y) {
    for (int x = 0; x < line.width; ++x) {
      if (line.pixels[static_cast<std::size_t>(y) * line.width + x] > 127) {
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  CHECK(bottom + 1 == line.baseline);
  CHECK(std::abs((line.baseline - top) - static_cast<int>(cap)) <= 1);
  // Anti-aliased: edge texels between ink and blank.
  const auto partial =
      std::count_if(line.pixels.begin(), line.pixels.end(),
                    [](uint8_t v) { return v > 0 && v < 255; });
  CHECK(partial > 0);
}

TEST_CASE("HD typeface narrows a line that would not fit") {
  auto t = chakra();
  const std::u32string text = U"RESTAURANT POPULARITY";
  const float natural = t->measure(text, 60, 3);
  const auto wide = t->line(text, 60, 3, natural + 50);
  const auto narrow = t->line(text, 60, 3, natural * 0.7f);
  CHECK(narrow.width < wide.width);
  CHECK(narrow.width <= static_cast<int>(natural * 0.7f) + 8);
  CHECK(narrow.height == wide.height); // narrowed, not shrunk
}

TEST_CASE("HD text keeps the layout of the original, in the HD picture only") {
  test_support::ScopedHdScale scale(4);
  HdTypefaces::set_enabled(true);
  const TextStyle style{.color = gfx::ColorIndex{15}};

  Surface plain(120, 20, gfx::PaletteHandle{0}, 3);
  auto original = block_font();
  const auto m0 = original->render_line_onto(plain, "AB 12", {4, 4}, style);

  Surface hd(120, 20, gfx::PaletteHandle{0}, 3);
  auto font = block_font();
  font->attach_hd(chakra(),
                  Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
  const auto m1 = font->render_line_onto(hd, "AB 12", {4, 4}, style);

  CHECK(m1.width == m0.width);
  CHECK(m1.height == m0.height);
  CHECK(m1.render_x == m0.render_x);
  // The 1994 letters would show through around the HD ones: the indexed
  // pixels are left alone and the letters are in the HD picture.
  CHECK(std::all_of(hd.pixels().begin(), hd.pixels().end(),
                    [](uint8_t p) { return p == 3; }));
  REQUIRE(hd.has_hd());
  const HdImage* out = hd.hd();
  REQUIRE(out);
  CHECK(std::any_of(out->pixels.begin(), out->pixels.end(),
                    [](uint32_t p) { return gfx::alpha(p) == 255; }));
}

TEST_CASE("HD text is drawn over the HD picture under it") {
  test_support::ScopedHdScale scale(4);
  HdTypefaces::set_enabled(true);
  const int k = 4;
  Surface s(60, 16, gfx::PaletteHandle{0}, 3);
  // A green HD picture under the whole surface.
  auto bg = std::make_shared<HdImage>(60 * k, 16 * k,
                                      gfx::pack_rgba(10, 200, 30, 255));
  s.attach_hd(bg);
  auto font = block_font();
  font->attach_hd(chakra(),
                  Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
  const TextStyle style{.color = gfx::ColorIndex{15}};
  font->render_line_onto(s, "HI", {2, 2}, style);

  const HdImage* out = s.hd();
  REQUIRE(out);
  // Outside the letters the green picture is untouched, inside the line's
  // block too (between the letters), not replaced by indexed pixels.
  CHECK(out->at(59 * k, 15 * k) == gfx::pack_rgba(10, 200, 30, 255));
  int green = 0;
  int other = 0;
  for (int y = 2 * k; y < 12 * k; ++y) {
    for (int x = 2 * k; x < 20 * k; ++x) {
      (out->at(x, y) == gfx::pack_rgba(10, 200, 30, 255) ? green : other)++;
    }
  }
  CHECK(green > 0);
  CHECK(other > 0);
  // Smooth edges: many in-between colours, not two flat ones.
  CHECK(count_colours(*out) > 8);
}

TEST_CASE("HD text off draws the original letters only") {
  test_support::ScopedHdScale scale(4);
  HdTypefaces::set_enabled(false);
  Surface s(60, 16, gfx::PaletteHandle{0}, 3);
  auto font = block_font();
  font->attach_hd(chakra(),
                  Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
  font->render_line_onto(s, "HI", {2, 2},
                         TextStyle{.color = gfx::ColorIndex{15}});
  CHECK_FALSE(s.has_hd());
  HdTypefaces::set_enabled(true);
}

TEST_CASE("HD typeface table loads the shipped fonts") {
  const auto dir = fonts_dir();
  REQUIRE_FALSE(dir.empty());
  HdTypefaces::instance().configure(dir);
  for (const char* name : {"ASTANDAR", "CSTANDAR", "MENU", "PIZ-NUMB", "RUBY",
                           "SUPERSMA", "MINI"}) {
    INFO(name);
    CHECK(HdTypefaces::instance().get(name) != nullptr);
  }
  CHECK(HdTypefaces::instance().get("NOPE") == nullptr);
  HdTypefaces::instance().configure({});
  CHECK(HdTypefaces::instance().get("MENU") == nullptr);
}

TEST_CASE(
    "HD text leaves no original letters showing around narrower HD ones") {
  test_support::ScopedHdScale scale(4);
  HdTypefaces::set_enabled(true);
  const int k = 4;
  const uint32_t green = gfx::pack_rgba(10, 200, 30, 255);
  Surface s(200, 16, gfx::PaletteHandle{0}, 3);
  s.attach_hd(std::make_shared<HdImage>(200 * k, 16 * k, green));
  auto font = block_font(); // blocks 8 pixels apart: far wider than "i"s
  font->attach_hd(chakra(),
                  Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
  const TextStyle style{.color = gfx::ColorIndex{15},
                        .align = TextAnchor::Middle};
  const auto m = font->render_line_onto(s, "iiiiiiii", {100, 2}, style);
  REQUIRE(m.width > 40);
  const HdImage* out = s.hd();
  REQUIRE(out);
  // The first and last original block are well outside the narrow HD
  // letters: there the HD picture must still be the green background.
  for (int x : {m.render_x * k + 2, (m.render_x + m.width) * k - 3}) {
    CHECK(out->at(x, 5 * k) == green);
  }
}

TEST_CASE("HD text from glyph sheets matches the per-line pictures") {
  test_support::ScopedHdScale scale(4);
  HdTypefaces::set_enabled(true);
  const int k = 4;
  const uint32_t green = gfx::pack_rgba(10, 200, 30, 255);
  const TextStyle style{
      .color = gfx::ColorIndex{15},
      .shadow = ShadowStyle{.offset = {1, 1}, .color = gfx::ColorIndex{1}}};
  auto typeface = chakra();
  auto draw = [&](bool sheets) {
    HdTypefaces::set_sheets_enabled(sheets);
    Surface s(120, 20, gfx::PaletteHandle{0}, 3);
    s.attach_hd(std::make_shared<HdImage>(120 * k, 20 * k, green));
    auto font = block_font();
    font->attach_hd(typeface,
                    Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
    typeface->wait_for_prewarm();
    font->render_line_onto(s, "Pizza 42!", {4, 4}, style);
    HdTypefaces::set_sheets_enabled(true);
    REQUIRE(s.hd_list());
    return std::make_pair(s.hd_list()->cmds().size(), *s.hd());
  };
  const auto [quads, from_sheet] = draw(true);
  const auto [pictures, from_picture] = draw(false);
  // A quad per letter (and per letter of the shadow) against one picture.
  CHECK(quads > 10);
  CHECK(pictures == 2); // the green background and the line
  REQUIRE(from_sheet.pixels.size() == from_picture.pixels.size());
  int worst = 0;
  std::size_t different = 0;
  for (std::size_t i = 0; i < from_sheet.pixels.size(); ++i) {
    const uint32_t a = from_sheet.pixels[i];
    const uint32_t b = from_picture.pixels[i];
    int d = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      d = std::max(d, std::abs(static_cast<int>((a >> shift) & 0xFF) -
                               static_cast<int>((b >> shift) & 0xFF)));
    }
    worst = std::max(worst, d);
    different += d > 2 ? 1 : 0;
  }
  // Only where two letters' edges overlap (blended there, the larger
  // coverage in the picture) may a texel differ.
  CHECK(different * 200 < from_sheet.pixels.size());
  INFO("worst " << worst);
  CHECK(worst <= 128);
}

TEST_CASE("HD typeface makes its glyph sheet in the background") {
  auto typeface = chakra();
  CHECK(typeface->sheet(40.0f) == nullptr);
  typeface->prewarm(40.0f);
  typeface->wait_for_prewarm();
  const auto sheet = typeface->sheet(40.0f);
  REQUIRE(sheet);
  REQUIRE(sheet->image);
  CHECK(sheet->slots.size() == 95 * 2);
  std::vector<std::shared_ptr<const HdImage>> taken;
  typeface->take_new_sheets(taken);
  REQUIRE(taken.size() == 1);
  CHECK(taken.front() == sheet->image);
  taken.clear();
  typeface->take_new_sheets(taken);
  CHECK(taken.empty());
  // A line of ASCII lays out from the sheet; letters outside it don't.
  const auto ascii = typeface->line(U"Pizza", 40.0f, 0.0f, 1e6f);
  CHECK(ascii.from_sheet);
  CHECK(ascii.glyphs.size() == 5);
  for (const auto& g : ascii.glyphs) {
    const auto& slot = sheet->slots[static_cast<std::size_t>(g.slot)];
    CHECK(slot.w > 0);
    CHECK(g.x >= 0);
    CHECK(g.x + slot.w <= ascii.width);
  }
  CHECK_FALSE(typeface->line(U"Pizza", 40.0f, 0.0f, 20.0f).from_sheet);
}

TEST_CASE("HD typeface moves letters closer before narrowing them") {
  auto t = chakra();
  const std::u32string text = U"RESTAURANT POPULARITY";
  const float natural = t->measure(text, 60, 3);
  // A little too wide: tighter spacing is enough, letters keep their shape.
  const auto tight = t->line(text, 60, 3, natural * 0.97f);
  CHECK(tight.squeeze == 1.0f);
  CHECK(tight.from_sheet);
  CHECK(tight.width <= static_cast<int>(natural * 0.97f) + 8);
  // Far too wide: narrowed as well.
  const auto narrow = t->line(text, 60, 3, natural * 0.6f);
  CHECK(narrow.squeeze < 1.0f);
  CHECK_FALSE(narrow.from_sheet);
  CHECK(narrow.width <= static_cast<int>(natural * 0.6f) + 8);
}

TEST_CASE("HD text cost", "[.bench]") {
  test_support::ScopedHdScale scale(6);
  HdTypefaces::set_enabled(true);
  const int k = 6;
  auto font = block_font();
  font->attach_hd(chakra(),
                  Font::HdMetrics{.cap_top = 0, .baseline = 5, .scale = 2});
  auto bg = std::make_shared<HdImage>(640 * k, 400 * k,
                                      gfx::pack_rgba(20, 20, 20, 255));
  (void)gfx::opaque_block(*bg, 0, 0, k); // as the renderer would have by now
  using clock = std::chrono::steady_clock;
  const char* env = std::getenv("PLEGACY_BENCH_FRAMES");
  const int frames = env ? std::max(2, std::atoi(env)) : 20;
  double ms = 0;
  double first = 0;
  for (int f = 0; f < frames; ++f) {
    Surface panel(640, 400, gfx::PaletteHandle{0}, 3);
    panel.attach_hd(bg);
    const auto t0 = clock::now();
    for (int line = 0; line < 15; ++line) {
      font->render_line_onto(panel, "POPULARITY    4  WAITING TIME",
                             {420, 20 + line * 12},
                             TextStyle{.color = gfx::ColorIndex{15}});
    }
    const double d =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    (f == 0 ? first : ms) += d;
  }
  std::cout << "15 HD text lines at hd_scale 6: first draw " << first
            << " ms, then " << ms / (frames - 1) << " ms per redraw\n";
}
