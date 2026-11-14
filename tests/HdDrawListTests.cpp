/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// HD draw lists (HdList) must give exactly the texels the old CPU texel-copy
// code gave. These tests run random drawing on both and compare every texel.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <vector>

#include "HdReference.h"
#include "TestSupport.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

using namespace gfx;
using hdref::RefSurface;
using test_support::ScopedHdScale;

namespace {

struct Pair {
  Surface s;
  RefSurface r;
};

uint32_t random_texel(std::mt19937& rng) {
  std::uniform_int_distribution<int> c(0, 255);
  const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
  const uint32_t a = kind < 5   ? 255u
                     : kind < 7 ? 0u
                                : static_cast<uint32_t>(c(rng));
  return pack_rgba(static_cast<uint32_t>(c(rng)), static_cast<uint32_t>(c(rng)),
                   static_cast<uint32_t>(c(rng)), a);
}

Pair make_pair(std::mt19937& rng, int k, int w, int h, int kind) {
  Pair p{Surface(w, h, PaletteHandle{0}, 0), RefSurface(w, h, k, 0)};
  std::uniform_int_distribution<int> idx(0, 7);
  if (kind == 0) {
    return p; // empty canvas
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto v = static_cast<uint8_t>(idx(rng) < 3 ? 0 : idx(rng) + 1);
      p.s.put(ViewPoint{x, y}, v);
      p.r.put(x, y, v);
    }
  }
  if (kind == 1) {
    return p; // indexed only
  }
  auto img = std::make_shared<HdImage>(w * k, h * k, 0u);
  // Blocky texels, so whole engine pixels are often fully opaque.
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint32_t block = random_texel(rng);
      const bool mixed = idx(rng) == 0;
      for (int ty = 0; ty < k; ++ty) {
        for (int tx = 0; tx < k; ++tx) {
          img->at(x * k + tx, y * k + ty) = mixed ? random_texel(rng) : block;
        }
      }
    }
  }
  p.r.hd = *img;
  p.s.attach_hd(img);
  return p;
}

uint32_t norm(uint32_t t) { return alpha(t) == 0 ? 0u : t; }

// Blending the pictures one by one instead of pre-merged rounds differently
// (blending is associative, fixed-point rounding is not), so allow a few
// steps per channel. A wrong rule gives differences far larger than this.
constexpr int tolerance = 2; // seen: 1, on under 0.01% of texels
bool close(uint32_t a, uint32_t b) {
  auto near = [](uint32_t x, uint32_t y) {
    return (x > y ? x - y : y - x) <= static_cast<uint32_t>(tolerance);
  };
  return near(red(a), red(b)) && near(green(a), green(b)) &&
         near(blue(a), blue(b)) && near(alpha(a), alpha(b));
}

void require_same(const Pair& p, int k, int step) {
  INFO("step " << step);
  for (int y = 0; y < p.r.h; ++y) {
    for (int x = 0; x < p.r.w; ++x) {
      REQUIRE(p.s.get(ViewPoint{x, y}) == p.r.get(x, y));
    }
  }
  const HdImage* got = p.s.hd();
  for (int ty = 0; ty < p.r.h * k; ++ty) {
    for (int tx = 0; tx < p.r.w * k; ++tx) {
      const uint32_t want = p.r.hd ? norm(p.r.hd->at(tx, ty)) : 0u;
      const uint32_t have = got ? norm(got->at(tx, ty)) : 0u;
      if (!close(have, want)) {
        INFO("texel " << tx << "," << ty << " have " << std::hex << have
                      << " want " << want);
        REQUIRE(close(have, want));
      }
    }
  }
}

void run_random_drawing(int k, uint32_t seed, int steps) {
  ScopedHdScale scale(k);
  std::mt19937 rng(seed);
  auto rnd = [&](int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  };
  std::vector<Pair> pool;
  for (int i = 0; i < 8; ++i) {
    pool.push_back(make_pair(rng, k, rnd(4, 24), rnd(4, 20),
                             i % 4 == 0   ? 0
                             : i % 4 == 1 ? 1
                                          : 2));
  }
  for (int step = 0; step < steps; ++step) {
    const int di = rnd(0, static_cast<int>(pool.size()) - 1);
    int si = rnd(0, static_cast<int>(pool.size()) - 1);
    if (si == di) {
      si = (si + 1) % static_cast<int>(pool.size());
    }
    Pair& d = pool[static_cast<std::size_t>(di)];
    const Pair& s = pool[static_cast<std::size_t>(si)];
    const int op = rnd(0, 13);
    const int sx = rnd(-3, s.r.w);
    const int sy = rnd(-3, s.r.h);
    const int sw = rnd(0, s.r.w + 3);
    const int sh = rnd(0, s.r.h + 3);
    const int dx = rnd(-6, d.r.w);
    const int dy = rnd(-6, d.r.h);
    const auto key = static_cast<uint8_t>(rnd(0, 9) == 0 ? rnd(1, 8) : 0);
    switch (op) {
    case 0:
    case 1:
    case 2:
    case 3:
      d.s.blit(s.s, ViewRect{sx, sy, sw, sh}, ViewPoint{dx, dy}, key);
      d.r.blit(s.r, sx, sy, sw, sh, dx, dy, key);
      break;
    case 4:
    case 5:
      d.s.blit_masked(s.s, ViewPoint{dx, dy});
      d.r.blit(s.r, 0, 0, s.r.w, s.r.h, dx, dy, 0);
      break;
    case 6:
      d.s.blit_raw(s.s, ViewPoint{dx, dy});
      d.r.blit_raw(s.r, dx, dy);
      break;
    case 7:
      d.s.blit_stencil(s.s, ViewRect{sx, sy, sw, sh}, ViewPoint{dx, dy}, 3,
                       key);
      d.r.blit_stencil(s.r, sx, sy, sw, sh, dx, dy, 3, key);
      break;
    case 8: {
      // Whole 2x2 blocks inside the source (the old code read past the
      // source's edge otherwise).
      const int hx = rnd(0, s.r.w / 2 - 1) * 2;
      const int hy = rnd(0, s.r.h / 2 - 1) * 2;
      const int hw = rnd(1, (s.r.w - hx) / 2) * 2;
      const int hh = rnd(1, (s.r.h - hy) / 2) * 2;
      d.s.blit_half(s.s, ViewRect{hx, hy, hw, hh}, ViewPoint{dx, dy});
      d.r.blit_half(s.r, hx, hy, hw, hh, dx, dy);
      break;
    }
    case 9:
      for (int i = 0; i < 6; ++i) {
        const int x = rnd(-1, d.r.w);
        const int y = rnd(-1, d.r.h);
        const auto v = static_cast<uint8_t>(rnd(0, 8));
        d.s.put(ViewPoint{x, y}, v);
        d.r.put(x, y, v);
      }
      break;
    case 10: {
      const auto v = static_cast<uint8_t>(rnd(0, 8));
      d.s.fill_rect(ViewRect{dx, dy, sw, sh}, v);
      d.r.fill_rect(dx, dy, sw, sh, v);
      break;
    }
    case 11:
      if (rnd(0, 3) == 0) {
        const auto v = static_cast<uint8_t>(rnd(0, 3));
        d.s.fill(v);
        d.r.fill(v);
      }
      else {
        // Restart this slot as a fresh HD picture, to keep sources varied.
        d = make_pair(rng, k, rnd(4, 24), rnd(4, 20), 2);
      }
      break;
    case 12:
      d.s.replace_colors({2, 5}, 6);
      d.r.replace_colors({2, 5}, 6);
      break;
    case 13: {
      Pair copy{s.s.clone(), s.r};
      d = std::move(copy);
      break;
    }
    }
    if (step % 7 == 0 || step == steps - 1) {
      require_same(d, k, step);
    }
  }
  for (const Pair& p : pool) {
    require_same(p, k, steps);
  }
}

} // namespace

TEST_CASE("HD draw lists match the texel-copy rules", "[hd][drawlist]") {
  resources::load_fallback_palette();
  for (int k : {2, 3, 4}) {
    for (uint32_t seed = 1; seed <= 12; ++seed) {
      INFO("k " << k << " seed " << seed);
      run_random_drawing(k, seed * 7919u + static_cast<uint32_t>(k), 400);
    }
  }
}

TEST_CASE("redrawing without clearing keeps the list small", "[hd][drawlist]") {
  ScopedHdScale scale(2);
  std::mt19937 rng(5);
  Pair sprite = make_pair(rng, 2, 6, 6, 2);
  Surface canvas(40, 30, PaletteHandle{0}, 1);
  RefSurface ref(40, 30, 2, 1);
  for (int frame = 0; frame < 3000; ++frame) {
    const int x = frame % 30;
    canvas.blit_raw(sprite.s, ViewPoint{x, 10});
    ref.blit_raw(sprite.r, x, 10);
  }
  REQUIRE(canvas.hd_list() != nullptr);
  CHECK(canvas.hd_list()->cmds().size() < 1200);
  Pair check{std::move(canvas), ref};
  require_same(check, 2, 3000);
}

TEST_CASE("an opaque HD picture needs no indexed redraw under it",
          "[hd][drawlist]") {
  ScopedHdScale scale(2);
  Surface sprite(4, 4, PaletteHandle{0}, 5);
  sprite.attach_hd(std::make_shared<HdImage>(8, 8, pack_rgba(1, 2, 3, 255)));
  Surface soft(4, 4, PaletteHandle{0}, 5);
  soft.attach_hd(std::make_shared<HdImage>(8, 8, pack_rgba(1, 2, 3, 100)));
  Surface layer(10, 10, PaletteHandle{0}, 0);
  layer.put(ViewPoint{0, 0}, 1); // plain pixel before any HD
  layer.blit_masked(sprite, ViewPoint{1, 1});
  layer.blit_masked(soft, ViewPoint{6, 6});
  const HdList* list = layer.hd_list();
  REQUIRE(list != nullptr);
  REQUIRE(list->cmds().size() == 2);
  CHECK(list->floor_of(2, 2) == 0);
  CHECK_FALSE(list->needs_base(2, 2)); // opaque copy covers it
  CHECK(list->floor_of(7, 7) == 1);
  CHECK(list->needs_base(7, 7)); // soft texels: indexed pixel shows through
}
