/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Hidden by default ("[.bench]"); run with: test_plegacy "[.bench]"
// Measures what one fully dirty HD frame costs: an HD background, 60 HD
// sprites on it, and the expansion to RGBA the renderer does.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <iostream>
#include <memory>

#include "TestSupport.h"
#include "engine/Renderer.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

using namespace gfx;

TEST_CASE("HD frame cost", "[.bench]") {
  resources::load_fallback_palette();
  for (int k : {1, 2, 3, 4, 6}) {
    test_support::ScopedHdScale scale(k);
    Surface background(640, 400, PaletteHandle{0}, 3);
    Surface sprite(48, 56, PaletteHandle{0}, 7);
    if (k > 1) {
      background.attach_hd(std::make_shared<HdImage>(
          640 * k, 400 * k, pack_rgba(90, 60, 30, 255)));
      sprite.attach_hd(std::make_shared<HdImage>(48 * k, 56 * k,
                                                 pack_rgba(20, 200, 40, 200)));
    }
    constexpr int frames = 10;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
      Surface layer(640, 400);
      layer.blit_raw(background);
      for (int i = 0; i < 60; ++i) {
        layer.blit_masked(sprite, ViewPoint{(i * 37) % 590, (i * 53) % 340});
      }
      auto rgba = Renderer::to_sdl_surface(layer, 1.0f, k);
      REQUIRE(rgba);
    }
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count() /
                    frames;
    std::cout << "hd_scale " << k << " (" << 640 * k << "x" << 400 * k
              << "): " << ms << " ms per fully redrawn frame\n";
  }
}
