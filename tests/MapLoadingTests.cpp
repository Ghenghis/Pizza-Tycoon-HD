/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Emissive and normal maps read off the game thread (fx::MapStore's
// background loading): a lookup never reads a file; maps are queued when
// their picture is bound or queued by the pack's prefetch; a map not read,
// or not on the GPU yet, draws nothing that frame and the glow appears a
// few frames later exactly as the blocking path draws it. Also the music
// accent's input (MusicAccent.h).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "engine/HdCompositor.h"
#include "engine/Renderable.h"
#include "engine/Renderer.h"
#include "engine/SDLContext.h"
#include "engine/SDL_include.h"
#include "engine/fx/Look.h"
#include "engine/fx/MapStore.h"
#include "engine/fx/MusicAccent.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/HdPack.h"
#include "resources/PaletteLoader.h"

namespace fs = std::filesystem;
using test_support::ScopedHdScale;

namespace {

// The maps folder for one test, read in the background (the default).
struct BackgroundMaps {
  explicit BackgroundMaps(const fs::path& root) {
    fx::MapStore::instance().loading(fx::MapStore::Loading::Background);
    fx::MapStore::instance().configure(root);
  }
  ~BackgroundMaps() { fx::MapStore::instance().configure({}); }
  BackgroundMaps(const BackgroundMaps&) = delete;
  BackgroundMaps& operator=(const BackgroundMaps&) = delete;
};

// A w x h map, `colour` left of x0, transparent right of it.
gfx::HdImage half_mask(int w, int h, int x0, uint32_t colour) {
  gfx::HdImage m(w, h, 0u);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < x0; ++x) {
      m.at(x, y) = colour;
    }
  }
  return m;
}

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

  // One frame as the game draws it (nothing forced) at full night glow.
  std::vector<uint32_t> frame() {
    fx::FrameState state;
    state.emissive = 1.0f;
    fx::set_frame_state(state);
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

} // namespace

TEST_CASE("a map lookup never reads a file; the worker does", "[fx][maps]") {
  ScopedHdScale scale(6);
  test_support::TempDir dir;
  REQUIRE(
      gfx::save_png(half_mask(60, 60, 30, gfx::pack_rgba(255, 220, 90, 255)),
                    dir.path() / "test" / "pane" / "emissive.png"));
  BackgroundMaps maps(dir.path());
  auto& store = fx::MapStore::instance();
  REQUIRE_FALSE(store.blocking());
  auto picture = std::make_shared<const gfx::HdImage>(60, 60, 0xFFu);
  const std::size_t generation = store.generation();
  store.bind(picture, "test/pane"); // queues its maps
  // Not taken in yet: nothing this frame, however fast the worker is.
  CHECK(store.emissive(picture.get()) == nullptr);
  CHECK(store.normal(picture.get()) == nullptr);
  store.wait_for_loads();
  CHECK(store.pending() == 0);
  CHECK(store.generation() != generation);
  const auto map = store.emissive(picture.get());
  REQUIRE(map);
  CHECK(map->width == 60);
  CHECK(gfx::alpha(map->at(30, 30)) > 0);        // feathered by the worker
  CHECK(store.normal(picture.get()) == nullptr); // there is none
  CHECK(store.emissive(picture.get()) == map);   // settled
}

TEST_CASE("maps are queued with the pack's prefetch", "[fx][maps]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const fs::path pack = dir.path();
  const uint32_t warm = gfx::pack_rgba(255, 180, 60, 255);
  // A map whose picture is not in the pack: only the prefetch's queue can
  // have asked for it.
  REQUIRE(gfx::save_png(gfx::HdImage(384, 88, warm),
                        pack / "_maps" / "gfx" / "k3" / "emissive.png"));
  REQUIRE(gfx::save_png(gfx::HdImage(8, 8, warm), pack / "gfx" / "other.png"));
  auto& hd = resources::HdPack::instance();
  hd.configure(pack);
  BackgroundMaps maps({}); // the pack's own _maps folder
  auto& store = fx::MapStore::instance();
  REQUIRE(store.enabled());

  hd.prefetch({{.relative = "gfx/k3.png", .engine_size = Size{192, 44}}},
              std::size_t{64} << 20);
  hd.wait_for_prefetch();
  store.sync(); // sees the prefetch's list and queues its maps
  store.wait_for_loads();

  auto picture = std::make_shared<const gfx::HdImage>(384, 88, 0xFFu);
  store.bind(picture, "gfx/k3");
  CHECK(store.pending() == 0); // read already: nothing queued again
  const auto map = store.emissive(picture.get());
  REQUIRE(map);
  CHECK(map->at(0, 0) == warm);

  (void)hd.take_prefetched();
  hd.configure({});
}

TEST_CASE("a map not ready draws nothing, then the same glow as blocking",
          "[fx][maps][hd]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  const uint32_t grey = gfx::pack_rgba(60, 60, 60, 255);
  auto picture = std::make_shared<const gfx::HdImage>(1280, 800, grey);
  REQUIRE(
      gfx::save_png(half_mask(1280, 800, 640, gfx::pack_rgba(200, 150, 0, 255)),
                    dir.path() / "test" / "sign" / "emissive.png"));

  std::vector<uint32_t> plain;
  {
    BackgroundMaps none(dir.path() / "none");
    SoftScreen screen(picture);
    plain = screen.frame();
  }
  std::vector<uint32_t> blocking;
  {
    BackgroundMaps maps(dir.path());
    const fx::MapStore::ScopedBlocking still;
    fx::MapStore::instance().bind(picture, "test/sign");
    SoftScreen screen(picture);
    blocking = screen.frame();
  }
  REQUIRE(blocking != plain);

  BackgroundMaps maps(dir.path());
  fx::MapStore::instance().bind(picture, "test/sign");
  SoftScreen screen(picture);
  // The first frame: the map is at best read, never on the GPU yet.
  CHECK(screen.frame() == plain);
  fx::MapStore::instance().wait_for_loads();
  std::vector<uint32_t> shown = plain;
  int frames = 0;
  while (shown != blocking && frames < 16) {
    shown = screen.frame();
    ++frames;
  }
  CHECK(shown == blocking);
  CHECK(frames <= 6);
  CHECK(screen.renderer->compositor()->map_uploads() >= 1);
}

TEST_CASE("clearing the maps drops what is queued", "[fx][maps]") {
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  for (int i = 0; i < 8; ++i) {
    REQUIRE(
        gfx::save_png(gfx::HdImage(64, 64, 0xFFFFFFFFu),
                      dir.path() / "m" / std::to_string(i) / "emissive.png"));
  }
  std::vector<std::shared_ptr<const gfx::HdImage>> pictures;
  {
    BackgroundMaps maps(dir.path());
    for (int i = 0; i < 8; ++i) {
      pictures.push_back(std::make_shared<const gfx::HdImage>(64, 64, 0xFFu));
      fx::MapStore::instance().bind(pictures.back(), "m/" + std::to_string(i));
    }
  } // configure({}) stops the worker with work possibly left
  CHECK(fx::MapStore::instance().pending() == 0);
  CHECK(fx::MapStore::instance().emissive(pictures[0].get()) == nullptr);
}

TEST_CASE("the music level is safe from any thread and clamped",
          "[fx][ambient]") {
  fx::set_music_level(0.5f);
  CHECK(fx::music_level() == 0.5f);
  fx::set_music_level(3.0f);
  CHECK(fx::music_level() == 1.0f);
  fx::set_music_level(-1.0f);
  CHECK(fx::music_level() == 0.0f);
  fx::set_music_level(std::numeric_limits<float>::quiet_NaN());
  CHECK(fx::music_level() == 0.0f);
  // As an audio callback would: another thread, many times.
  std::thread meter([] {
    for (int i = 0; i <= 1000; ++i) {
      fx::set_music_level(static_cast<float>(i) / 1000.0f);
    }
  });
  meter.join();
  CHECK(fx::music_level() == 1.0f);
  fx::set_music_level(0.0f);
}
