/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "TestSupport.h"
#include "engine/Renderer.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

using namespace gfx;
using test_support::ScopedHdScale;

namespace {

constexpr int K = 3;
const uint32_t GREEN = pack_rgba(0, 255, 0, 255);

// An 8x8 sprite whose index pixels are 5 inside a 1-pixel transparent border,
// with HD texels that are solid green inside and a soft half-alpha ring that
// spills one pixel past the original outline.
Surface make_hd_sprite() {
  Surface s(8, 8, PaletteHandle{0}, 0);
  s.fill_rect(ViewRect{1, 1, 6, 6}, 5);
  auto hd = std::make_shared<HdImage>(8 * K, 8 * K, 0u);
  for (int y = 0; y < 8 * K; ++y) {
    for (int x = 0; x < 8 * K; ++x) {
      const bool inside = x >= K && x < 7 * K && y >= K && y < 7 * K;
      hd->at(x, y) = inside ? GREEN : pack_rgba(0, 255, 0, 128);
    }
  }
  s.attach_hd(hd);
  return s;
}

} // namespace

TEST_CASE("surfaces without HD are untouched by HD code", "[hd][surface]") {
  ScopedHdScale scale(K);
  Surface dst(16, 16, PaletteHandle{0}, 1);
  Surface src(4, 4, PaletteHandle{0}, 7);
  dst.blit_masked(src, ViewPoint{2, 2});
  dst.put(ViewPoint{0, 0}, 9);
  dst.fill_rect(ViewRect{10, 10, 3, 3}, 4);
  CHECK_FALSE(dst.has_hd());
  CHECK(dst.get(ViewPoint{3, 3}) == 7);
}

TEST_CASE("attach_hd rejects a wrong size", "[hd][surface]") {
  ScopedHdScale scale(K);
  Surface s(4, 4);
  // fatal() exits the process, so only the right size is exercised here; the
  // HD pack resamples before attaching, which the pack tests cover.
  s.attach_hd(std::make_shared<HdImage>(4 * K, 4 * K, GREEN));
  REQUIRE(s.has_hd());
  CHECK(s.hd()->width == 12);
}

TEST_CASE("masked blit of an HD sprite carries its texels", "[hd][surface]") {
  ScopedHdScale scale(K);
  const Surface sprite = make_hd_sprite();
  Surface dst(20, 20, PaletteHandle{0}, 2);
  dst.blit_masked(sprite, ViewPoint{4, 4});

  REQUIRE(dst.has_hd());
  // Index pixels are copied as before.
  CHECK(dst.get(ViewPoint{5, 5}) == 5);
  CHECK(dst.get(ViewPoint{4, 4}) == 2); // transparent border kept the floor
  // Inside the outline the HD texels replaced whatever was there.
  CHECK(dst.hd()->at(5 * K + 1, 5 * K + 1) == GREEN);
  // On the transparent border the soft ring was blended onto "nothing", so it
  // keeps its half alpha and is laid over the floor when presented.
  CHECK(alpha(dst.hd()->at(4 * K, 4 * K)) == 128);
  // Outside the sprite nothing changed.
  CHECK(dst.hd()->at(0, 0) == 0u);
}

TEST_CASE("plain pixels drawn over HD let the original show through",
          "[hd][surface]") {
  ScopedHdScale scale(K);
  const Surface sprite = make_hd_sprite();
  Surface dst(20, 20, PaletteHandle{0}, 2);
  dst.blit_masked(sprite, ViewPoint{0, 0});
  REQUIRE(alpha(dst.hd()->at(3 * K, 3 * K)) == 255);

  SECTION("put") {
    dst.put(ViewPoint{3, 3}, 9);
    CHECK(dst.hd()->at(3 * K, 3 * K) == 0u);
    CHECK(dst.hd()->at(3 * K + K - 1, 3 * K + K - 1) == 0u);
    CHECK(alpha(dst.hd()->at(4 * K, 4 * K)) == 255); // neighbour untouched
  }
  SECTION("fill_rect") {
    dst.fill_rect(ViewRect{2, 2, 2, 2}, 9);
    CHECK(dst.hd()->at(2 * K, 2 * K) == 0u);
    CHECK(dst.hd()->at(4 * K - 1, 4 * K - 1) == 0u);
    CHECK(alpha(dst.hd()->at(4 * K, 4 * K)) == 255);
  }
  SECTION("masked blit of a plain sprite") {
    Surface text(2, 1, PaletteHandle{0}, 0);
    text.put(ViewPoint{0, 0}, 11); // second pixel stays transparent
    dst.blit_masked(text, ViewPoint{3, 3});
    CHECK(dst.hd()->at(3 * K, 3 * K) == 0u);
    CHECK(alpha(dst.hd()->at(4 * K, 3 * K)) == 255);
  }
  SECTION("stencil") {
    Surface shape(1, 1, PaletteHandle{0}, 3);
    dst.blit_stencil(shape, shape.rect(), ViewPoint{3, 3}, 12);
    CHECK(dst.hd()->at(3 * K, 3 * K) == 0u);
  }
  SECTION("replace_colors only clears pixels that changed") {
    dst.replace_colors({5}, 6);
    CHECK(dst.hd()->at(3 * K, 3 * K) == 0u); // was 5, now 6
    CHECK(alpha(dst.hd()->at(0, 0)) == 128); // floor pixel, unchanged
  }
  SECTION("fill drops the HD layer") {
    dst.fill(0);
    CHECK_FALSE(dst.has_hd());
  }
}

TEST_CASE("blit_raw copies or clears HD texels", "[hd][surface]") {
  ScopedHdScale scale(K);
  const Surface sprite = make_hd_sprite();
  Surface dst(20, 20, PaletteHandle{0}, 2);

  dst.blit_raw(sprite, ViewPoint{-2, 10}); // clipped on the left
  REQUIRE(dst.has_hd());
  CHECK(dst.hd()->at(0, 10 * K) == sprite.hd()->at(2 * K, 0));
  CHECK(dst.hd()->at(3 * K, 13 * K) == GREEN);

  Surface plain(4, 4, PaletteHandle{0}, 8);
  dst.blit_raw(plain, ViewPoint{0, 10});
  CHECK(dst.hd()->at(3 * K, 13 * K) == 0u);
}

TEST_CASE("HD images are copy-on-write", "[hd][surface]") {
  ScopedHdScale scale(K);
  const Surface sprite = make_hd_sprite();
  Surface copy = sprite.clone();
  REQUIRE(copy.hd() == sprite.hd()); // shared
  copy.put(ViewPoint{3, 3}, 1);
  CHECK(copy.hd() != sprite.hd());
  CHECK(sprite.hd()->at(3 * K, 3 * K) == GREEN);
  CHECK(copy.hd()->at(3 * K, 3 * K) == 0u);
}

TEST_CASE("blit_half averages HD blocks", "[hd][surface]") {
  ScopedHdScale scale(K);
  Surface src(4, 4, PaletteHandle{0}, 5);
  auto hd = std::make_shared<HdImage>(4 * K, 4 * K, GREEN);
  src.attach_hd(hd);
  Surface dst(2, 2, PaletteHandle{0}, 0);
  dst.blit_half(src, src.rect(), ViewPoint{0, 0});
  CHECK(dst.get(ViewPoint{1, 1}) == 5);
  REQUIRE(dst.has_hd());
  CHECK(dst.hd()->at(2 * K - 1, 2 * K - 1) == GREEN);
}

TEST_CASE("renderer expands to HD and lays texels over the index pixels",
          "[hd][renderer]") {
  ScopedHdScale scale(K);
  resources::load_fallback_palette();
  Surface layer(10, 10, PaletteHandle{0}, 0);
  layer.fill_rect(ViewRect{0, 0, 10, 10}, 1);
  layer.blit_masked(make_hd_sprite(), ViewPoint{1, 1});

  auto out = Renderer::to_sdl_surface(layer, 1.0f, K);
  REQUIRE(out);
  CHECK(out->w == 10 * K);
  CHECK(out->h == 10 * K);
  auto texel = [&](int x, int y) {
    return static_cast<const uint32_t*>(
        out->pixels)[static_cast<std::size_t>(y) * (out->pitch / 4) + x];
  };
  // Inside the sprite: pure HD green.
  CHECK(texel(4 * K, 4 * K) == GREEN);
  // Plain floor far away: the palette colour of index 1, opaque.
  const auto& c = layer.colors()[1];
  CHECK(texel(0, 0) == pack_rgba(c.r, c.g, c.b, 255));
  // Index 0 is transparent in HD just like in the original path.
  Surface empty(2, 2, PaletteHandle{0}, 0);
  auto blank = Renderer::to_sdl_surface(empty, 1.0f, K);
  CHECK(static_cast<const uint32_t*>(blank->pixels)[0] == 0u);

  SECTION("fades darken HD texels too") {
    auto dark = Renderer::to_sdl_surface(layer, 0.0f, K);
    const uint32_t t = static_cast<const uint32_t*>(
        dark->pixels)[static_cast<std::size_t>(4 * K) * (dark->pitch / 4) +
                      4 * K];
    CHECK(red(t) == 0);
    CHECK(green(t) == 0);
    CHECK(blue(t) == 0);
  }
}

TEST_CASE("soft HD texels blend once on every texel row", "[hd][renderer]") {
  // Rows after the first of each engine pixel used to be blended twice.
  resources::load_fallback_palette();
  ScopedHdScale scale(K);
  Surface soft(2, 2, PaletteHandle{0}, 25);
  soft.attach_hd(
      std::make_shared<HdImage>(2 * K, 2 * K, pack_rgba(200, 10, 10, 100)));
  auto rgba = Renderer::to_sdl_surface(soft, 1.0f, K);
  const auto* first = static_cast<const uint32_t*>(rgba->pixels);
  for (int ty = 1; ty < K; ++ty) {
    const auto* row = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(rgba->pixels) + ty * rgba->pitch);
    CHECK(row[0] == first[0]);
  }
}
