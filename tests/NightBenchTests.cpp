/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Hidden by default; run with: test_plegacy "[.bench][night]"
//
// The night look's cost at hd_scale 6 (3840 x 2400), Vivid at 23:00, at the
// North Pole: the city map (glow with twinkle, aurora, snow) and the
// restaurant (glow with twinkle). For each:
//   * the first night frame, maps read and uploaded on the game thread
//     (MapStore's blocking loading, as before background loading) and with
//     background loading (read, resized and feathered by the worker,
//     uploaded within the per-frame budget), plus how many frames the glow
//     then takes to appear;
//   * the steady frame time with nothing, with the glow, plus the aurora,
//     plus the snow.
//
// The maps: PLEGACY_BENCH_MAPS names a maps folder (<asset id>/emissive.png,
// e.g. the curator's emissive-stage _maps); each of its maps is drawn over
// a picture of its own size. Without it, a synthetic pack: a full-screen
// background and its map plus 48 city tiles (16 with maps) or 30 pieces of
// furniture (10 with maps), written to a temporary folder.
//
// This runs on SDL's software renderer (no window, no GPU): the numbers are
// the CPU raster's, an upper bound. The GPU's are the game's own bench
// (data/review/bench-gpu.exec).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "TestSupport.h"
#include "engine/HdCompositor.h"
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
#include "resources/PaletteLoader.h"

namespace fs = std::filesystem;
using test_support::ScopedHdScale;

namespace {

constexpr int k = 6;
constexpr int frame_w = 640 * k;
constexpr int frame_h = 400 * k;

struct Piece {
  std::shared_ptr<const gfx::HdImage> picture;
  std::string id;
  int x = 0; // texels
  int y = 0;
};

class SceneView : public Renderable {
public:
  explicit SceneView(const std::vector<Piece>& pieces) : pieces_(pieces) {}
  void render(const RenderContext& ctx) override {
    for (const Piece& p : pieces_) {
      gfx::HdCmd c;
      c.img = p.picture;
      c.sw = p.picture->width;
      c.sh = p.picture->height;
      c.dx = p.x;
      c.dy = p.y;
      ctx.surface->hd_overlay(c, 0, 0, frame_w, frame_h);
    }
  }
  bool is_dirty() const override { return dirty_; }
  void clean() override { dirty_ = false; }
  void mark_dirty() override { dirty_ = true; }
  bool needs_force_recompose() const override { return false; }
  void tick() override {}
  std::vector<LayerInfo> get_layers() override {
    return {LayerInfo{this, ViewRect{0, 0, 640, 400}, "scene"}};
  }

private:
  const std::vector<Piece>& pieces_;
  bool dirty_ = true;
};

// Lit windows: a few bright rectangles on a transparent map.
gfx::HdImage window_map(int w, int h, uint32_t seed) {
  gfx::HdImage m(w, h, 0u);
  const int cols = std::max(1, w / 60);
  const int rows = std::max(1, h / 80);
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < cols; ++i) {
      const uint32_t hash =
          (seed * 2654435761u) ^ ((i * 73856093u) ^ (j * 19349663u));
      if (hash % 3 != 0) {
        continue;
      }
      const int x0 = i * w / cols + w / cols / 4;
      const int y0 = j * h / rows + h / rows / 4;
      for (int y = y0; y < y0 + h / rows / 2 && y < h; ++y) {
        for (int x = x0; x < x0 + w / cols / 2 && x < w; ++x) {
          m.at(x, y) = gfx::pack_rgba(255, 200, 110, 230);
        }
      }
    }
  }
  return m;
}

struct Pack {
  fs::path maps;
  std::vector<Piece> city;
  std::vector<Piece> restaurant;
  std::size_t map_count = 0;
};

std::shared_ptr<const gfx::HdImage> solid(int w, int h, uint32_t colour) {
  return std::make_shared<const gfx::HdImage>(w, h, colour);
}

// The synthetic pack, in `dir`.
Pack synthetic_pack(const fs::path& dir) {
  Pack pack;
  pack.maps = dir;
  auto add = [&](std::vector<Piece>& scene, const std::string& id, int w, int h,
                 int x, int y, bool lit, uint32_t colour) {
    scene.push_back({solid(w, h, colour), id, x, y});
    if (lit) {
      REQUIRE(
          gfx::save_png(window_map(w, h, static_cast<uint32_t>(scene.size())),
                        dir / id / "emissive.png"));
      ++pack.map_count;
    }
  };
  // The city map: the screen, then 8 x 6 tiles of 40 x 40 in the map's
  // 320 x 240 viewport at (40, 94).
  add(pack.city, "bench/city/back", frame_w, frame_h, 0, 0, true,
      gfx::pack_rgba(20, 30, 50, 255));
  for (int t = 0; t < 48; ++t) {
    add(pack.city, "bench/city/tile" + std::to_string(t), 40 * k, 40 * k,
        (40 + (t % 8) * 40) * k, (94 + (t / 8) * 40) * k, t % 3 == 0,
        gfx::pack_rgba(70, 60, 50, 255));
  }
  // The restaurant: the room, then 30 pieces of 48 x 56.
  add(pack.restaurant, "bench/room/back", frame_w, frame_h, 0, 0, true,
      gfx::pack_rgba(60, 40, 30, 255));
  for (int f = 0; f < 30; ++f) {
    add(pack.restaurant, "bench/room/piece" + std::to_string(f), 48 * k, 56 * k,
        ((f * 97) % 580) * k, (60 + (f * 53) % 320) * k, f % 3 == 0,
        gfx::pack_rgba(110, 80, 60, 255));
  }
  return pack;
}

// A real maps folder: every <id>/emissive.png over a picture of its size,
// laid out left to right, top to bottom, in both scenes.
Pack folder_pack(const fs::path& maps) {
  Pack pack;
  pack.maps = maps;
  std::vector<std::pair<std::string, gfx::PngInfo>> found;
  for (const auto& e : fs::recursive_directory_iterator(maps)) {
    if (e.is_regular_file() && e.path().filename() == "emissive.png") {
      if (auto info = gfx::read_png_info(e.path())) {
        const std::string id =
            fs::relative(e.path().parent_path(), maps).generic_string();
        found.emplace_back(id, *info);
      }
    }
  }
  std::ranges::sort(found, {}, &std::pair<std::string, gfx::PngInfo>::first);
  int x = 0;
  int y = 0;
  int row_h = 0;
  for (const auto& [id, info] : found) {
    const int w = info.width;
    const int h = info.height;
    if (x > 0 && x + w > frame_w) {
      x = 0;
      y += row_h;
      row_h = 0;
    }
    if (y >= frame_h) {
      y = 0;
    }
    for (auto* scene : {&pack.city, &pack.restaurant}) {
      scene->push_back(
          {solid(w, h, gfx::pack_rgba(60, 50, 40, 255)), id, x, y});
    }
    x += w;
    row_h = std::max(row_h, h);
    ++pack.map_count;
  }
  return pack;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

struct Screen {
  SDL_Surface* canvas =
      SDL_CreateSurface(frame_w, frame_h, SDL_PIXELFORMAT_RGBA8888);
  SDLContext sdl;
  SceneView view;
  std::unique_ptr<Renderer> renderer;

  explicit Screen(const std::vector<Piece>& pieces) : view(pieces) {
    sdl.renderer.reset(SDL_CreateSoftwareRenderer(canvas));
    REQUIRE(sdl.renderer);
    sdl.texture.reset(
        SDL_CreateTexture(sdl.renderer.get(), SDL_PIXELFORMAT_RGBA8888,
                          SDL_TEXTUREACCESS_TARGET, frame_w, frame_h));
    renderer = std::make_unique<Renderer>(sdl, 400);
    renderer->add_layers(&view);
  }
  ~Screen() {
    fx::set_frame_state(fx::FrameState{});
    renderer.reset();
    sdl.texture.reset();
    sdl.renderer.reset();
    SDL_DestroySurface(canvas);
  }
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  double frame(const fx::FrameState& state) {
    fx::set_frame_state(state);
    const auto t0 = std::chrono::steady_clock::now();
    renderer->render();
    return ms_since(t0);
  }
};

fx::FrameState night_state(fx::Area area, double t) {
  fx::LookSettings look;
  look.preset = fx::Preset::Vivid;
  look.ambient_motion = fx::AmbientMotion::Full;
  return fx::resolve_frame(look, {.area = area,
                                  .hours = 23.0f,
                                  .anim_seconds = t,
                                  .location = "north-pole",
                                  .map_rect = {40, 94, 320, 240}});
}

// The first night frame (after one by day, so the art itself is on the
// GPU), then frames until the glow is complete.
void first_night(const char* name, const Pack& pack,
                 const std::vector<Piece>& pieces, fx::Area area,
                 fx::MapStore::Loading loading) {
  auto& store = fx::MapStore::instance();
  store.loading(loading);
  store.configure(pack.maps);
  Screen screen(pieces);
  const auto t0 = std::chrono::steady_clock::now();
  for (const Piece& p : pieces) {
    store.bind(p.picture, p.id); // as sync() does when the pack loads them
  }
  const double bind_ms = ms_since(t0);
  fx::FrameState day = night_state(area, 0.0);
  day.emissive = 0.0f;
  day.aurora = 0.0f;
  day.twinkle = false;
  day.weather = fx::Weather::None;
  const double day_ms = screen.frame(day);
  const double night_ms = screen.frame(night_state(area, 0.0));
  // Until everything is read and on the GPU: nothing queued, nothing
  // streaming, and a frame that took in no map.
  auto maps_seen = [&] {
    return store.generation() + screen.renderer->compositor()->map_uploads();
  };
  int frames = 1;
  double worst = night_ms;
  std::size_t seen = maps_seen();
  const auto settle = std::chrono::steady_clock::now();
  while (frames < 2000) {
    const bool busy =
        store.pending() > 0 || screen.renderer->compositor()->streaming() > 0;
    worst = std::max(worst, screen.frame(night_state(area, frames / 60.0)));
    ++frames;
    const std::size_t now = maps_seen();
    if (!busy && now == seen) {
      break;
    }
    seen = now;
  }
  const double settle_ms = ms_since(settle);
  std::cout << "  " << name << " first night frame, "
            << (loading == fx::MapStore::Loading::Blocking
                    ? "maps on the game thread (before): "
                    : "maps in the background (after):   ")
            << night_ms << " ms (binding " << bind_ms << " ms, day frame "
            << day_ms << " ms); glow complete after " << frames << " frames, "
            << settle_ms << " ms, worst frame " << worst << " ms\n";
  store.configure({});
  store.loading(fx::MapStore::Loading::Background);
}

void steady(const char* name, const Pack& pack,
            const std::vector<Piece>& pieces, fx::Area area) {
  auto& store = fx::MapStore::instance();
  store.loading(fx::MapStore::Loading::Blocking); // everything ready
  store.configure(pack.maps);
  for (const Piece& p : pieces) {
    store.bind(p.picture, p.id);
  }
  Screen screen(pieces);
  constexpr int frames = 60;
  auto run = [&](bool glow, bool aurora, bool snow) {
    double total = 0;
    for (int i = -5; i < frames; ++i) {
      fx::FrameState s = night_state(area, 100.0 + i / 60.0);
      if (!glow) {
        s.emissive = 0.0f;
        s.twinkle = false;
      }
      if (!aurora) {
        s.aurora = 0.0f;
      }
      if (!snow) {
        s.weather = fx::Weather::None;
      }
      const double ms = screen.frame(s);
      if (i >= 0) {
        total += ms;
      }
    }
    return total / frames;
  };
  const double none = run(false, false, false);
  const double glow = run(true, false, false);
  const double with_aurora = run(true, true, false);
  const double all = run(true, true, true);
  std::cout << "  " << name << " steady frame: nothing " << none
            << " ms, glow + twinkle " << glow << " ms (+" << glow - none
            << "), + aurora " << with_aurora << " ms (+" << with_aurora - glow
            << "), + snow " << all << " ms (+" << all - with_aurora
            << "); effects in all +" << all - none << " ms\n";
  store.configure({});
  store.loading(fx::MapStore::Loading::Background);
}

} // namespace

TEST_CASE("the night look's cost at hd_scale 6", "[.bench][night]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(k);
  test_support::TempDir dir;
  const char* env = std::getenv("PLEGACY_BENCH_MAPS");
  const bool real = env && *env;
  const Pack pack = real ? folder_pack(env) : synthetic_pack(dir.path());
  REQUIRE(pack.map_count > 0);
  std::cout << "Night look, hd_scale " << k << " (" << frame_w << "x" << frame_h
            << "), Vivid 23:00, North Pole, software renderer; "
            << pack.map_count << " emissive maps"
            << (real ? " from " + std::string(env) : " (synthetic)") << "\n";
  for (const auto& [name, pieces, area] :
       {std::tuple{"city map", &pack.city, fx::Area::City},
        std::tuple{"restaurant", &pack.restaurant, fx::Area::Restaurant}}) {
    first_night(name, pack, *pieces, area, fx::MapStore::Loading::Blocking);
    first_night(name, pack, *pieces, area, fx::MapStore::Loading::Background);
    steady(name, pack, *pieces, area);
  }
  CHECK(fx::MapStore::instance().pending() == 0);
}
