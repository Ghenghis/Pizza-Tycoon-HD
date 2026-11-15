/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Emissive maps at run time: drawn additively over their picture at the
// night factor (game clock times the preset's glow), nothing by day or
// without a map, feathered at high scales, and taken per location through
// the location art resolver.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "TestSupport.h"
#include "engine/Renderable.h"
#include "engine/Renderer.h"
#include "engine/SDLContext.h"
#include "engine/SDL_include.h"
#include "engine/fx/Look.h"
#include "engine/fx/MapStore.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/HdPack.h"
#include "resources/PaletteLoader.h"
#include "views/hd/LocationArt.h"

namespace fs = std::filesystem;
using test_support::ScopedHdScale;

namespace {

// One full-screen layer: a fixed grey HD picture (the same object every
// frame, so the maps know it).
class SpriteView : public Renderable {
public:
  explicit SpriteView(std::shared_ptr<const gfx::HdImage> picture)
      : picture_(std::move(picture)) {}
  void render(const RenderContext& ctx) override {
    gfx::HdCmd c;
    c.img = picture_;
    c.sw = picture_->width;
    c.sh = picture_->height;
    ctx.surface->hd_overlay(c, 0, 0, picture_->width, picture_->height);
  }
  bool is_dirty() const override { return dirty_; }
  void clean() override { dirty_ = false; }
  void mark_dirty() override { dirty_ = true; }
  bool needs_force_recompose() const override { return false; }
  void tick() override {}
  std::vector<LayerInfo> get_layers() override {
    return {LayerInfo{this, ViewRect{0, 0, 640, 400}, "sprite"}};
  }

private:
  std::shared_ptr<const gfx::HdImage> picture_;
  bool dirty_ = true;
};

// A software-rendered Renderer at hd_scale 2 (1280 x 800).
struct SoftScreen {
  SDL_Surface* canvas = SDL_CreateSurface(1280, 800, SDL_PIXELFORMAT_RGBA8888);
  SDLContext sdl;
  SpriteView view;
  std::unique_ptr<Renderer> renderer;

  explicit SoftScreen(std::shared_ptr<const gfx::HdImage> picture)
      : view(std::move(picture)) {
    resources::load_fallback_palette();
    sdl.renderer.reset(SDL_CreateSoftwareRenderer(canvas));
    REQUIRE(sdl.renderer);
    sdl.texture.reset(SDL_CreateTexture(sdl.renderer.get(),
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, 1280, 800));
    renderer = std::make_unique<Renderer>(sdl, 400);
    renderer->add_layers(&view);
  }
  ~SoftScreen() {
    renderer.reset();
    sdl.texture.reset();
    sdl.renderer.reset();
    SDL_DestroySurface(canvas);
  }
  SoftScreen(const SoftScreen&) = delete;
  SoftScreen& operator=(const SoftScreen&) = delete;

  // The composited frame (lighting and glow included) at `emissive`.
  std::vector<uint32_t> frame(float emissive) {
    fx::FrameState state;
    state.emissive = emissive;
    fx::set_frame_state(state);
    renderer->redraw_all();
    renderer->render();
    std::vector<uint32_t> out;
    renderer->with_current_frame([&out](SDL_Surface* s) {
      out.resize(static_cast<std::size_t>(s->w) * s->h);
      for (int y = 0; y < s->h; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * s->w,
                    static_cast<const uint8_t*>(s->pixels) + y * s->pitch,
                    static_cast<std::size_t>(s->w) * 4);
      }
    });
    fx::set_frame_state(fx::FrameState{});
    return out;
  }
};

struct ScopedMaps {
  explicit ScopedMaps(const fs::path& root) {
    fx::MapStore::instance().configure(root);
  }
  ~ScopedMaps() { fx::MapStore::instance().configure({}); }
  ScopedMaps(const ScopedMaps&) = delete;
  ScopedMaps& operator=(const ScopedMaps&) = delete;
};

// A w x h map, (r, g, b) left of x0, transparent right of it.
gfx::HdImage half_mask(int w, int h, int x0, uint32_t colour) {
  gfx::HdImage m(w, h, 0u);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < x0; ++x) {
      m.at(x, y) = colour;
    }
  }
  return m;
}

int channel(uint32_t p, int shift) {
  return static_cast<int>((p >> shift) & 0xFF);
}

} // namespace

TEST_CASE("the emissive feather softens edges without bleeding colour",
          "[fx][emissive]") {
  CHECK(fx::emissive_feather_radius(2) == 0);
  CHECK(fx::emissive_feather_radius(5) == 0);
  CHECK(fx::emissive_feather_radius(6) == 3);
  CHECK(fx::emissive_feather_radius(8) == 4);

  gfx::HdImage square(40, 40, 0u);
  const uint32_t amber = gfx::pack_rgba(255, 200, 0, 255);
  for (int y = 10; y < 30; ++y) {
    for (int x = 10; x < 30; ++x) {
      square.at(x, y) = amber;
    }
  }
  gfx::HdImage same = square;
  fx::feather(same, 0);
  CHECK(same.pixels == square.pixels);

  gfx::HdImage soft = square;
  fx::feather(soft, 3);
  CHECK(soft.at(20, 20) == amber); // the middle stays as it was
  CHECK(gfx::alpha(soft.at(0, 0)) == 0);
  const uint32_t edge = soft.at(10, 20);
  CHECK(gfx::alpha(edge) > 0);
  CHECK(gfx::alpha(edge) < 255);
  CHECK(gfx::alpha(soft.at(8, 20)) > 0); // spills past the old edge
  // Premultiplied: the soft edge keeps the glow's colour (no dark fringe).
  CHECK(std::abs(static_cast<int>(gfx::red(edge)) - 255) <= 2);
  CHECK(std::abs(static_cast<int>(gfx::green(edge)) - 200) <= 2);
  gfx::HdImage again = square;
  fx::feather(again, 3);
  CHECK(again.pixels == soft.pixels);
}

TEST_CASE("emissive maps are feathered on load from hd_scale 6",
          "[fx][emissive]") {
  test_support::TempDir dir;
  REQUIRE(
      gfx::save_png(half_mask(60, 60, 30, gfx::pack_rgba(255, 220, 90, 255)),
                    dir.path() / "test" / "pane" / "emissive.png"));
  for (int k : {2, 6}) {
    INFO("hd_scale " << k);
    ScopedHdScale scale(k);
    ScopedMaps maps(dir.path());
    auto picture = std::make_shared<const gfx::HdImage>(60, 60, 0xFFu);
    fx::MapStore::instance().bind(picture, "test/pane");
    const auto map = fx::MapStore::instance().emissive(picture.get());
    REQUIRE(map);
    const uint32_t a = gfx::alpha(map->at(30, 30)); // first texel off
    if (k == 2) {
      CHECK(a == 0); // a hard edge
    }
    else {
      CHECK(a > 0); // feathered
    }
  }
}

TEST_CASE("a night factor adds the emissive map, by day nothing",
          "[fx][emissive][hd]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const uint32_t grey = gfx::pack_rgba(60, 60, 60, 255);
  auto picture = std::make_shared<const gfx::HdImage>(1280, 800, grey);

  std::vector<uint32_t> plain;
  {
    // No maps at all.
    ScopedMaps maps(dir.path() / "none");
    SoftScreen screen(picture);
    plain = screen.frame(0.0f);
    CHECK(screen.frame(0.5f) == plain); // no map: nothing at any night
  }

  REQUIRE(
      gfx::save_png(half_mask(1280, 800, 640, gfx::pack_rgba(200, 150, 0, 255)),
                    dir.path() / "maps" / "test" / "sign" / "emissive.png"));
  ScopedMaps maps(dir.path() / "maps");
  fx::MapStore::instance().bind(picture, "test/sign");
  SoftScreen screen(picture);
  CHECK(screen.frame(0.0f) == plain); // by day: the same as no map

  const auto half = screen.frame(0.5f);
  const auto full = screen.frame(1.0f);
  for (const std::size_t i :
       {std::size_t{400} * 1280 + 100, std::size_t{10} * 1280 + 600}) {
    // Left: grey plus half the glow; with full glow, clamped at white.
    CHECK(std::abs(channel(half[i], 24) - (60 + 100)) <= 1);
    CHECK(std::abs(channel(half[i], 16) - (60 + 75)) <= 1);
    CHECK(channel(half[i], 8) == 60);
    CHECK(channel(full[i], 24) == 255);
    CHECK(std::abs(channel(full[i], 16) - 210) <= 1);
  }
  // Right of the map's edge: no glow.
  const std::size_t right = std::size_t{400} * 1280 + 1000;
  CHECK(half[right] == plain[right]);
  CHECK(full[right] == plain[right]);
}

TEST_CASE("the night factor comes from the clock and the preset",
          "[fx][emissive]") {
  fx::LookSettings look;
  const fx::FrameContext night{.area = fx::Area::Restaurant, .hours = 23.0f};
  const fx::FrameContext noon{.area = fx::Area::Restaurant, .hours = 12.0f};
  CHECK(fx::resolve_frame(look, night).emissive == 0.0f); // Classic
  look.preset = fx::Preset::Vivid;
  CHECK(fx::resolve_frame(look, night).emissive == 1.0f);
  CHECK(fx::resolve_frame(look, noon).emissive == 0.0f);
  look.preset = fx::Preset::Studio;
  CHECK(fx::resolve_frame(look, night).emissive ==
        fx::preset_grade(fx::Preset::Studio).glow);
  // Places without a clock never glow.
  CHECK(fx::resolve_frame(look, {.area = fx::Area::Other, .hours = 23.0f})
            .emissive == 0.0f);
  // The "Day and night" option is not needed for it.
  CHECK_FALSE(look.day_night);
}

TEST_CASE("a location's picture takes the location's emissive map",
          "[fx][emissive][location]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const fs::path pack = dir.path();
  const uint32_t warm = gfx::pack_rgba(255, 180, 60, 255);
  const uint32_t aurora = gfx::pack_rgba(60, 255, 180, 255);
  REQUIRE(gfx::save_png(gfx::HdImage(384, 88, gfx::pack_rgba(5, 5, 5, 255)),
                        pack / "gfx" / "k3.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(384, 88, gfx::pack_rgba(9, 9, 9, 255)),
                        pack / "locations" / "north-pole" / "gfx" / "k3.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(384, 88, warm),
                        pack / "_maps" / "gfx" / "k3" / "emissive.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(384, 88, aurora),
                        pack / "_maps" / "locations" / "north-pole" / "gfx" /
                            "k3" / "emissive.png"));
  auto& hd = resources::HdPack::instance();
  hd.configure(pack);
  views::hd::clear_location_pictures();
  ScopedMaps maps({}); // the pack's own _maps folder

  // The classic floor and the North Pole's, as the screens get them.
  const auto classic_hd = hd.vga("gfx/k3", Size{192, 44});
  REQUIRE(classic_hd);
  gfx::Surface classic(192, 44, gfx::PaletteHandle{0}, 9);
  classic.attach_hd(classic_hd);
  const gfx::Surface& north = views::hd::location_picture(
      classic, "gfx/k3",
      views::hd::ArtScope{.ns = "north-pole", .district = ""});
  REQUIRE(&north != &classic);

  auto& store = fx::MapStore::instance();
  store.sync(); // learns both pictures' asset ids from the pack
  const auto classic_map = store.emissive(classic.hd());
  const auto north_map = store.emissive(north.hd());
  REQUIRE(classic_map);
  REQUIRE(north_map);
  CHECK(classic_map->at(0, 0) == warm);
  CHECK(north_map->at(0, 0) == aurora);

  views::hd::clear_location_pictures();
  hd.configure({});
}
