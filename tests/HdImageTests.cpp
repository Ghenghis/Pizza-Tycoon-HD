/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "engine/gfx/HdImage.h"

using namespace gfx;

TEST_CASE("pack_rgba matches SDL RGBA8888 layout", "[hd]") {
  const uint32_t p = pack_rgba(0x12, 0x34, 0x56, 0x78);
  CHECK(p == 0x12345678u);
  CHECK(red(p) == 0x12);
  CHECK(green(p) == 0x34);
  CHECK(blue(p) == 0x56);
  CHECK(alpha(p) == 0x78);
}

TEST_CASE("blend_over follows Porter-Duff over", "[hd]") {
  const uint32_t red_opaque = pack_rgba(255, 0, 0, 255);
  const uint32_t blue_opaque = pack_rgba(0, 0, 255, 255);

  SECTION("opaque source wins") {
    CHECK(blend_over(red_opaque, blue_opaque) == red_opaque);
  }
  SECTION("transparent source leaves destination") {
    CHECK(blend_over(pack_rgba(9, 9, 9, 0), blue_opaque) == blue_opaque);
  }
  SECTION("onto nothing keeps the source with its alpha") {
    const uint32_t half = pack_rgba(200, 100, 50, 128);
    CHECK(blend_over(half, 0) == half);
  }
  SECTION("half over opaque mixes and stays opaque") {
    const uint32_t out = blend_over(pack_rgba(255, 0, 0, 128), blue_opaque);
    CHECK(alpha(out) == 255);
    CHECK(red(out) == 128);
    CHECK(blue(out) == 127);
    CHECK(green(out) == 0);
  }
  SECTION("half over half gives three quarters coverage") {
    const uint32_t out =
        blend_over(pack_rgba(255, 255, 255, 128), pack_rgba(0, 0, 0, 128));
    CHECK(alpha(out) >= 191);
    CHECK(alpha(out) <= 193);
  }
}

TEST_CASE("scale_rgb fades colour but not alpha", "[hd]") {
  const uint32_t out = scale_rgb(pack_rgba(200, 100, 50, 77), 0.5f);
  CHECK(out == pack_rgba(100, 50, 25, 77));
  CHECK(scale_rgb(pack_rgba(1, 2, 3, 4), 1.0f) == pack_rgba(1, 2, 3, 4));
  CHECK(scale_rgb(pack_rgba(10, 20, 30, 40), 0.0f) == pack_rgba(0, 0, 0, 40));
}

TEST_CASE("resample keeps flat colour flat in both directions", "[hd]") {
  const uint32_t c = pack_rgba(40, 120, 220, 255);
  HdImage src(7, 5, c);
  for (auto [w, h] : {std::pair{21, 15}, std::pair{3, 2}, std::pair{64, 3}}) {
    const HdImage out = resample(src, w, h);
    REQUIRE(out.width == w);
    REQUIRE(out.height == h);
    for (uint32_t p : out.pixels) {
      REQUIRE(p == c);
    }
  }
}

TEST_CASE("resample shrinking averages exactly", "[hd]") {
  HdImage src(4, 2);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      src.at(x, y) = ((x + y) % 2 == 0) ? pack_rgba(255, 255, 255, 255)
                                        : pack_rgba(0, 0, 0, 255);
    }
  }
  const HdImage out = resample(src, 2, 1);
  for (uint32_t p : out.pixels) {
    CHECK(alpha(p) == 255);
    CHECK(red(p) >= 127);
    CHECK(red(p) <= 128);
  }
}

TEST_CASE("resample does not bleed colour out of transparent texels", "[hd]") {
  // A white opaque texel next to a fully transparent *red* one: the red must
  // not show up anywhere, because its alpha is zero.
  HdImage src(2, 1);
  src.at(0, 0) = pack_rgba(255, 255, 255, 255);
  src.at(1, 0) = pack_rgba(255, 0, 0, 0);
  const HdImage out = resample(src, 8, 1);
  for (uint32_t p : out.pixels) {
    if (alpha(p) > 0) {
      CHECK(green(p) == 255);
      CHECK(blue(p) == 255);
    }
  }
  CHECK(alpha(out.at(0, 0)) == 255);
  CHECK(alpha(out.at(7, 0)) == 0);
}

TEST_CASE("hd_scale accepts 1 to 8", "[hd]") {
  const int saved = hd_scale();
  hd_scale(6);
  CHECK(hd_scale() == 6);
  hd_scale(saved);
}
