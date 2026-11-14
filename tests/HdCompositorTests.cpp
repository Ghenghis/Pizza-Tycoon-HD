/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The GPU path (HdCompositor) must draw what the CPU path draws. SDL's
// software renderer runs the same draw calls the GPU backends get, so the
// comparison runs on any machine.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <vector>

#include "TestSupport.h"
#include "engine/HdCompositor.h"
#include "engine/Renderer.h"
#include "engine/SDL_include.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

using namespace gfx;
using test_support::ScopedHdScale;

namespace {

struct SoftGpu {
  SDL_Surface* canvas;
  SDL_Renderer* renderer;
  explicit SoftGpu(int w, int h)
      : canvas(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888)),
        renderer(SDL_CreateSoftwareRenderer(canvas)) {}
  ~SoftGpu() {
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(canvas);
  }
};

std::shared_ptr<HdImage> random_image(std::mt19937& rng, int w, int h) {
  std::uniform_int_distribution<int> c(0, 255);
  std::uniform_int_distribution<int> kind(0, 9);
  auto img = std::make_shared<HdImage>(w, h, 0u);
  const int k = hd_scale();
  for (int by = 0; by < h / k; ++by) {
    for (int bx = 0; bx < w / k; ++bx) {
      const int t = kind(rng);
      const uint32_t a = t < 6   ? 255u
                         : t < 8 ? 0u
                                 : static_cast<uint32_t>(c(rng));
      const uint32_t v = pack_rgba(static_cast<uint32_t>(c(rng)),
                                   static_cast<uint32_t>(c(rng)),
                                   static_cast<uint32_t>(c(rng)), a);
      for (int y = 0; y < k; ++y) {
        for (int x = 0; x < k; ++x) {
          img->at(bx * k + x, by * k + y) = v;
        }
      }
    }
  }
  return img;
}

Surface random_sprite(std::mt19937& rng, int w, int h, bool hd) {
  Surface s(w, h, PaletteHandle{0}, 0);
  std::uniform_int_distribution<int> idx(0, 9);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int v = idx(rng);
      s.put(ViewPoint{x, y}, static_cast<uint8_t>(v < 3 ? 0 : v + 20));
    }
  }
  if (hd) {
    s.attach_hd(random_image(rng, w * hd_scale(), h * hd_scale()));
  }
  return s;
}

uint32_t premul(uint32_t p) {
  const uint32_t a = alpha(p);
  auto m = [a](uint32_t c) { return (c * a + 127) / 255; };
  return a == 0 ? 0u : pack_rgba(m(red(p)), m(green(p)), m(blue(p)), a);
}

// Largest per-channel difference between the GPU and the CPU result.
int compare(const Surface& layer, float fade) {
  const int k = hd_scale();
  const int w = layer.width() * k;
  const int h = layer.height() * k;
  SoftGpu gpu(w, h);
  REQUIRE(gpu.renderer != nullptr);
  HdCompositor comp(gpu.renderer);
  SDL_Texture* target = SDL_CreateTexture(
      gpu.renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
  REQUIRE(target != nullptr);
  comp.draw_layer(target, layer, fade);
  SDL_Surface* got_raw = SDL_RenderReadPixels(gpu.renderer, nullptr);
  REQUIRE(got_raw != nullptr);
  SDL_Surface* got = SDL_ConvertSurface(got_raw, SDL_PIXELFORMAT_RGBA8888);
  SDL_DestroySurface(got_raw);
  auto want = Renderer::to_sdl_surface(layer, fade, k);
  int worst = 0;
  for (int y = 0; y < h; ++y) {
    const auto* g = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(got->pixels) + y * got->pitch);
    const auto* e = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(want->pixels) + y * want->pitch);
    for (int x = 0; x < w; ++x) {
      const uint32_t a = g[x];
      const uint32_t b = premul(e[x]);
      auto d = [](uint32_t p, uint32_t q) {
        return static_cast<int>(p > q ? p - q : q - p);
      };
      worst = std::max({worst, d(red(a), red(b)), d(green(a), green(b)),
                        d(blue(a), blue(b)), d(alpha(a), alpha(b))});
    }
  }
  SDL_DestroySurface(got);
  SDL_DestroyTexture(target);
  return worst;
}

} // namespace

TEST_CASE("GPU compositing matches the CPU result", "[hd][gpu]") {
  resources::load_fallback_palette();
  for (int k : {2, 3}) {
    ScopedHdScale scale(k);
    for (uint32_t seed = 1; seed <= 25; ++seed) {
      INFO("k " << k << " seed " << seed);
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> pos(-6, 40);
      std::uniform_int_distribution<int> op(0, 9);
      std::vector<Surface> sprites;
      for (int i = 0; i < 5; ++i) {
        sprites.push_back(random_sprite(rng, 4 + i * 2, 5 + i, i != 4));
      }
      Surface room(48, 36, PaletteHandle{0}, 0);
      Surface layer(40, 30, PaletteHandle{0}, 0);
      room.blit_raw(random_sprite(rng, 48, 36, true), ViewPoint{0, 0});
      for (int n = 0; n < 40; ++n) {
        Surface& s = sprites[static_cast<std::size_t>(n) % sprites.size()];
        switch (op(rng)) {
        case 0:
          room.fill_rect(ViewRect{pos(rng), pos(rng), 5, 3}, 33);
          break;
        case 1:
          room.blit_half(s, ViewRect{0, 0, s.width(), s.height()},
                         ViewPoint{pos(rng), pos(rng)});
          break;
        case 2:
          room.put(ViewPoint{pos(rng), pos(rng)}, 0);
          break;
        default:
          room.blit_masked(s, ViewPoint{pos(rng), pos(rng)});
        }
      }
      layer.fill(7);
      layer.blit(room, ViewRect{4, 3, 40, 30}, ViewPoint{0, 0});
      layer.blit_masked(sprites[0], ViewPoint{10, 10});
      layer.fill_rect(ViewRect{30, 2, 6, 2}, 0); // a transparent hole
      CHECK(compare(layer, 1.0f) <= 2);
      CHECK(compare(layer, 0.5f) <= 3);
    }
  }
}

TEST_CASE("GPU textures are shared and released", "[hd][gpu]") {
  ScopedHdScale scale(2);
  SoftGpu gpu(64, 64);
  HdCompositor comp(gpu.renderer);
  auto img = std::make_shared<const HdImage>(8, 8, pack_rgba(1, 2, 3, 255));
  SDL_Texture* t1 = comp.texture_for(img);
  CHECK(comp.texture_for(img) == t1);
  CHECK(comp.texture_bytes() == 8 * 8 * 4);
  comp.end_frame();
  CHECK(comp.texture_count() == 1);
  img.reset();
  comp.end_frame();
  CHECK(comp.texture_count() == 0);
  CHECK(comp.texture_bytes() == 0);

  // Over budget: pictures not used in the current frame go first.
  comp.memory_budget(8 * 8 * 4);
  auto a = std::make_shared<const HdImage>(8, 8, 0u);
  auto b = std::make_shared<const HdImage>(8, 8, 0u);
  comp.texture_for(a);
  comp.end_frame();
  comp.texture_for(b);
  comp.end_frame();
  CHECK(comp.texture_count() == 1);
  CHECK(comp.preload({a, b}, 1 << 20) == 8 * 8 * 4);
}
