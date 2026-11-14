/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The GPU path (HdCompositor) must draw what the CPU path draws. SDL's
// software renderer runs the same draw calls the GPU backends get, so the
// comparison runs on any machine.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "TestSupport.h"
#include "engine/HdCompositor.h"
#include "engine/LightPass.h"
#include "engine/Lighting.h"
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

// The GPU result of draw_layer as RGBA8888 texels.
std::vector<uint32_t> gpu_pixels(const Surface& layer, int art_from_x) {
  const int k = hd_scale();
  const int w = layer.width() * k;
  const int h = layer.height() * k;
  SoftGpu gpu(w, h);
  HdCompositor comp(gpu.renderer);
  SDL_Texture* target = SDL_CreateTexture(
      gpu.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  comp.draw_layer(target, layer, 1.0f, art_from_x);
  SDL_Surface* raw = SDL_RenderReadPixels(gpu.renderer, nullptr);
  SDL_Surface* got = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA8888);
  std::vector<uint32_t> out(static_cast<std::size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    std::memcpy(out.data() + static_cast<std::size_t>(y) * w,
                static_cast<const uint8_t*>(got->pixels) + y * got->pitch,
                static_cast<std::size_t>(w) * 4);
  }
  SDL_DestroySurface(raw);
  SDL_DestroySurface(got);
  SDL_DestroyTexture(target);
  return out;
}

} // namespace

TEST_CASE("the 1994 look and the divider between the looks", "[hd][gpu]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  const int k = 2;
  std::mt19937 rng(21);
  Surface layer(60, 30, PaletteHandle{0}, 0);
  layer.blit_raw(random_sprite(rng, 60, 30, true), ViewPoint{0, 0});
  // A line of HD text: drawn on both sides.
  auto sheet = std::make_shared<HdImage>(20, 10, pack_rgba(255, 255, 255, 255));
  HdCmd letter;
  letter.img = sheet;
  letter.sw = 20;
  letter.sh = 10;
  letter.dx = 10;
  letter.dy = 20;
  letter.tint = pack_rgba(200, 10, 10, 255);
  layer.hd_overlay(letter, 0, 0, 120, 60);
  const int w = 60 * k;

  const auto hd = gpu_pixels(layer, 0);
  const auto classic = gpu_pixels(layer, w);
  CHECK(hd != classic);
  // Split at x = 30 engine pixels: left like the 1994 look, right like HD.
  const auto split = gpu_pixels(layer, 30 * k);
  int left_diff = 0;
  int right_diff = 0;
  for (int y = 0; y < 30 * k; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      if (x < 30 * k) {
        left_diff += split[i] != classic[i] ? 1 : 0;
      }
      else {
        right_diff += split[i] != hd[i] ? 1 : 0;
      }
    }
  }
  CHECK(left_diff == 0);
  CHECK(right_diff == 0);
  // The letters show in the 1994 look too (HD text is not art).
  CHECK(classic[static_cast<std::size_t>(22 * k) * w + 12 * k] ==
        hd[static_cast<std::size_t>(22 * k) * w + 12 * k]);
}

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

TEST_CASE("tinted coverage (HD text) draws the same on the GPU", "[hd][gpu]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(3);
  std::mt19937 rng(5);
  // A sheet of white letters: alpha is the coverage.
  auto sheet = std::make_shared<HdImage>(90, 30, 0u);
  std::uniform_int_distribution<int> cov(0, 255);
  for (uint32_t& p : sheet->pixels) {
    const int a = cov(rng);
    p = a < 60 ? 0u : pack_rgba(255, 255, 255, static_cast<uint32_t>(a));
  }
  Surface layer(80, 40, PaletteHandle{0}, 0);
  layer.blit_raw(random_sprite(rng, 80, 40, true), ViewPoint{0, 0});
  for (int i = 0; i < 30; ++i) {
    HdCmd cmd;
    cmd.img = sheet;
    cmd.sx = (i * 7) % 70;
    cmd.sy = (i * 5) % 20;
    cmd.sw = 17;
    cmd.sh = 10;
    // Anywhere, not on engine pixel edges, some over the surface's edge.
    cmd.dx = (i * 31) % 250 - 8;
    cmd.dy = (i * 13) % 125 - 4;
    cmd.tint = pack_rgba(static_cast<uint32_t>(cov(rng)),
                         static_cast<uint32_t>(cov(rng)),
                         static_cast<uint32_t>(cov(rng)), 255);
    layer.hd_overlay(cmd, 0, 0, 240, 120);
  }
  CHECK(layer.hd_list()->cmds().size() > 20);
  // The GPU multiplies the colour into premultiplied texels: one more
  // rounding step than the CPU's straight colours.
  CHECK(compare(layer, 1.0f) <= 3);
  CHECK(compare(layer, 0.5f) <= 4);
}

TEST_CASE("small pictures share atlas pages", "[hd][gpu]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  std::mt19937 rng(11);
  // Lines of text: small pictures marked for the atlas, some overlapping
  // and some cut by the layer's edge, over a background with HD art.
  std::vector<std::shared_ptr<const HdImage>> lines;
  {
    Surface layer(160, 100, PaletteHandle{0}, 0);
    layer.blit_raw(random_sprite(rng, 160, 100, true), ViewPoint{0, 0});
    for (int i = 0; i < 12; ++i) {
      const int w = 10 + i * 7;
      const int h = 5 + (i % 3) * 2;
      Surface line = random_sprite(rng, w, h, false);
      auto img = random_image(rng, w * 2, h * 2);
      img->atlas = true;
      line.attach_hd(img);
      lines.push_back(img);
      layer.blit(line, ViewRect{0, 0, w, h},
                 ViewPoint{(i * 23) % 150 - 5, (i * 9) % 95}, gfx::TRANSPARENT);
    }
    CHECK(compare(layer, 1.0f) <= 2);
    CHECK(compare(layer, 0.5f) <= 3);
  }

  SoftGpu gpu(64, 64);
  HdCompositor comp(gpu.renderer);
  for (const auto& img : lines) {
    CHECK(comp.texture_for(img) != nullptr);
  }
  CHECK(comp.picture_count() == lines.size());
  CHECK(comp.atlas_pages() == 1);
  CHECK(comp.texture_count() == 1);
  const auto a = comp.place(lines[0]);
  const auto b = comp.place(lines[1]);
  CHECK(a.tex == b.tex);
  CHECK((a.x != b.x || a.y != b.y));
  const std::size_t page_bytes = comp.texture_bytes();

  // Gone pictures leave the page; once it is empty it is filled again from
  // the top, without a new texture.
  lines.clear();
  comp.end_frame();
  CHECK(comp.picture_count() == 0);
  CHECK(comp.atlas_pages() == 1);
  auto again = std::make_shared<HdImage>(20, 10, pack_rgba(1, 2, 3, 255));
  again->atlas = true;
  const auto c = comp.place(again);
  CHECK(c.tex == a.tex);
  CHECK(c.x == 1);
  CHECK(c.y == 1);
  CHECK(comp.texture_bytes() == page_bytes);

  // Too big for a page: a texture of its own.
  auto big = std::make_shared<HdImage>(64, 2000, 0u);
  big->atlas = true;
  CHECK(comp.place(big).tex != a.tex);
  CHECK(comp.texture_count() == 2);
}

TEST_CASE("streamed textures draw the same as direct uploads", "[hd][gpu]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  std::mt19937 rng(3);
  // Big enough to be streamed in several bands.
  Surface big = random_sprite(rng, 1200, 600, true);
  Surface layer(1200, 600, PaletteHandle{0}, 0);
  layer.blit_raw(big, ViewPoint{0, 0});
  const int worst_direct = compare(layer, 1.0f);

  SoftGpu gpu(2400, 1200);
  HdCompositor comp(gpu.renderer);
  const auto img = layer.hd_list()->cmds().front().img;
  comp.stream({img}, 0.0); // queued, nothing uploaded yet
  CHECK(comp.streaming() == 1);
  comp.stream({}, 1e9); // all bands
  CHECK(comp.streaming() == 0);
  CHECK(comp.texture_bytes() == img->pixels.size() * 4);
  const double before = comp.upload_ms();
  CHECK(comp.texture_for(img) != nullptr);
  CHECK(comp.upload_ms() == before); // already complete: no upload at draw

  // A picture only partly streamed is finished when first drawn.
  auto other =
      std::make_shared<const HdImage>(2000, 900, pack_rgba(9, 8, 7, 255));
  comp.stream({other}, 0.0);
  CHECK(comp.texture_for(other) != nullptr);
  CHECK(worst_direct <= 2);
}

TEST_CASE("lights brighten their surroundings over the ambient level",
          "[hd][lighting]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  const int w = 64;
  const int h = 40;
  SoftGpu gpu(w * 2, h * 2);
  // A plain white layer.
  SDL_Texture* albedo =
      SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_Texture* out = SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_SetRenderTarget(gpu.renderer, albedo);
  SDL_SetRenderDrawColor(gpu.renderer, 255, 255, 255, 255);
  SDL_RenderClear(gpu.renderer);

  lighting::Scene scene;
  scene.ambient = 0.5f;
  lighting::Light lamp;
  lamp.x = 32;
  lamp.y = 20;
  lamp.radius = 12;
  scene.lights.push_back(lamp);
  LightPass pass(gpu.renderer);
  pass.apply(albedo, out, w, h, 2, scene, 0.0);
  SDL_Surface* raw = SDL_RenderReadPixels(gpu.renderer, nullptr);
  REQUIRE(raw);
  SDL_Surface* px = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA8888);
  auto red_at = [px](int x, int y) {
    const auto* row = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(px->pixels) + y * px->pitch);
    return static_cast<int>(red(row[x]));
  };
  CHECK(red_at(64, 40) >= 250);              // at the lamp: full brightness
  CHECK(std::abs(red_at(4, 4) - 128) <= 3);  // far away: the ambient level
  CHECK(red_at(64 + 16, 40) > red_at(4, 4)); // in between: lit, fading
  SDL_DestroySurface(px);
  SDL_DestroySurface(raw);
  SDL_DestroyTexture(out);
  SDL_DestroyTexture(albedo);
}

TEST_CASE("flicker stays in range and differs per seed", "[lighting]") {
  lighting::Light candle;
  candle.flicker = lighting::Flicker::Candle;
  candle.seed = 1;
  lighting::Light other = candle;
  other.seed = 99;
  bool differs = false;
  for (int i = 0; i < 200; ++i) {
    const double t = i * 0.05;
    const auto a = lighting::sample(candle, t);
    CHECK(a.r >= 0.0f);
    CHECK(a.r <= 1.0f);
    differs |= std::abs(a.r - lighting::sample(other, t).r) > 0.01f;
  }
  CHECK(differs);
  lighting::Scene scene;
  CHECK_FALSE(scene.active());
  scene.lights.push_back(candle);
  CHECK(scene.active());
  CHECK(scene.animated());
}
