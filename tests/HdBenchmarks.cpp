/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Hidden by default ("[.bench]"); run with: test_plegacy "[.bench]"
// Measures what one fully dirty HD frame costs on the CPU: an HD background
// and 60 HD sprites drawn on a layer. With GPU compositing (the default) that
// is all the CPU does; the CPU fallback (PLEGACY_CPU_COMPOSE=1) also
// flattens the draw list and expands the frame to RGBA.

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
    using clock = std::chrono::steady_clock;
    double record_ms = 0;
    double cpu_ms = 0;
    for (int f = 0; f < frames; ++f) {
      const auto t0 = clock::now();
      Surface layer(640, 400);
      layer.blit_raw(background);
      for (int i = 0; i < 60; ++i) {
        layer.blit_masked(sprite, ViewPoint{(i * 37) % 590, (i * 53) % 340});
      }
      const auto t1 = clock::now();
      auto rgba = Renderer::to_sdl_surface(layer, 1.0f, k);
      REQUIRE(rgba);
      const auto t2 = clock::now();
      record_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
      cpu_ms += std::chrono::duration<double, std::milli>(t2 - t0).count();
    }
    std::cout << "hd_scale " << k << " (" << 640 * k << "x" << 400 * k
              << "): GPU path CPU work " << record_ms / frames
              << " ms, CPU fallback " << cpu_ms / frames
              << " ms per fully redrawn frame\n";
  }
}
