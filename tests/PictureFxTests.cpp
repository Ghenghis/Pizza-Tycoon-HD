/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Phase 2 of the picture settings: day and night, street lamps, emissive
// night glow, snow and normal-map relief. Each is checked on its CPU
// reference or the software renderer; with the options off, or in the
// Classic preset, nothing changes.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "TestSupport.h"
#include "core/state/GameOptions.h"
#include "core/util/ConfigManager.h"
#include "engine/HdCompositor.h"
#include "engine/LightPass.h"
#include "engine/Lighting.h"
#include "engine/Random.h"
#include "engine/SDL_include.h"
#include "engine/fx/DayNight.h"
#include "engine/fx/Look.h"
#include "engine/fx/MapStore.h"
#include "engine/fx/NormalLight.h"
#include "engine/fx/Weather.h"
#include "engine/fx/generated/NormalLightShaders.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

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
  SoftGpu(const SoftGpu&) = delete;
  SoftGpu& operator=(const SoftGpu&) = delete;
  // The current target as RGBA8888 texels.
  [[nodiscard]] std::vector<uint32_t> read() const {
    SDL_Surface* raw = SDL_RenderReadPixels(renderer, nullptr);
    REQUIRE(raw);
    SDL_Surface* px = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA8888);
    REQUIRE(px);
    std::vector<uint32_t> out(static_cast<std::size_t>(px->w) * px->h);
    for (int y = 0; y < px->h; ++y) {
      std::memcpy(out.data() + static_cast<std::size_t>(y) * px->w,
                  static_cast<const uint8_t*>(px->pixels) + y * px->pitch,
                  static_cast<std::size_t>(px->w) * 4);
    }
    SDL_DestroySurface(px);
    SDL_DestroySurface(raw);
    return out;
  }
};

float red_of(uint32_t p) { return static_cast<float>(gfx::red(p)); }

// Puts the maps folder back to the pack's default when a test ends.
struct ScopedMaps {
  explicit ScopedMaps(const std::filesystem::path& root) {
    fx::MapStore::instance().configure(root);
  }
  ~ScopedMaps() { fx::MapStore::instance().configure({}); }
  ScopedMaps(const ScopedMaps&) = delete;
  ScopedMaps& operator=(const ScopedMaps&) = delete;
  // Maps read on first lookup, as these tests expect (the background
  // reading has tests of its own: MapLoadingTests.cpp).
  fx::MapStore::ScopedBlocking blocking;
};

// A hemisphere bump: normals lean out from the centre (G = +Y up).
gfx::HdImage bump(int size) {
  gfx::HdImage img(size, size, 0);
  const float c = (static_cast<float>(size) - 1.0f) / 2.0f;
  auto enc = [](float v) {
    return static_cast<uint32_t>(std::lround(128.0f + 127.0f * v));
  };
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float nx = (static_cast<float>(x) - c) / (c + 1.0f);
      const float ny = -(static_cast<float>(y) - c) / (c + 1.0f); // up
      const float r2 = std::min(0.95f, nx * nx + ny * ny);
      const float nz = std::sqrt(1.0f - r2);
      img.at(x, y) = gfx::pack_rgba(enc(nx), enc(ny), enc(nz), 255);
    }
  }
  return img;
}

} // namespace

TEST_CASE("the day and night curve", "[fx][daynight]") {
  for (float h : {8.0f, 10.0f, 12.0f, 15.5f, 17.0f}) {
    const fx::DayNight d = fx::day_night(h);
    CHECK(d.exposure == 1.0f);
    CHECK(d.warmth == 0.0f);
    CHECK(d.night == 0.0f);
  }
  const fx::DayNight night = fx::day_night(2.0f);
  CHECK(night.night == 1.0f);
  CHECK(night.exposure < 0.9f);
  CHECK(night.warmth < 0.0f);                 // blue
  CHECK(fx::day_night(6.5f).warmth > 0.0f);   // dawn: warm
  CHECK(fx::day_night(18.75f).warmth > 0.0f); // dusk: amber
  CHECK(fx::day_night(18.75f).exposure < 1.0f);
  CHECK(fx::day_night(0.0f) == fx::day_night(24.0f));
  CHECK(fx::day_night(-1.0f) == fx::day_night(23.0f));
  // Smooth: no jumps anywhere in the day.
  fx::DayNight prev = fx::day_night(0.0f);
  for (int i = 1; i <= 2400; ++i) {
    const fx::DayNight d = fx::day_night(static_cast<float>(i) / 100.0f);
    REQUIRE(std::abs(d.exposure - prev.exposure) < 0.01f);
    REQUIRE(std::abs(d.warmth - prev.warmth) < 0.02f);
    REQUIRE(std::abs(d.night - prev.night) < 0.02f);
    prev = d;
  }
}

TEST_CASE("day and night only touch what they should", "[fx][daynight]") {
  fx::LookSettings s;
  s.preset = fx::Preset::Natural;
  const fx::FrameContext city_night{.area = fx::Area::City, .hours = 23.0f};

  // Off: exactly as without it.
  fx::FrameState off = fx::resolve_frame(s, city_night);
  CHECK(off.grade == fx::resolve(s, fx::Area::City));
  CHECK(off.night == 0.0f);
  CHECK(off.sun_hours < 0.0f);

  s.day_night = true;
  const fx::FrameState on = fx::resolve_frame(s, city_night);
  CHECK(on.night == 1.0f);
  CHECK(on.grade.exposure < off.grade.exposure);
  CHECK(on.grade.warmth < off.grade.warmth);
  // By day the tint is exactly nothing.
  const fx::FrameState noon =
      fx::resolve_frame(s, {.area = fx::Area::City, .hours = 12.0f});
  CHECK(noon.grade == fx::resolve(s, fx::Area::City));
  CHECK(noon.night == 0.0f);
  // Rooms: the glow at night, but no sky tint.
  const fx::FrameState room =
      fx::resolve_frame(s, {.area = fx::Area::Restaurant, .hours = 23.0f});
  CHECK(room.night == 1.0f);
  CHECK(room.grade == fx::resolve(s, fx::Area::Restaurant));
  // Menus have no clock.
  CHECK(fx::resolve_frame(s, {.area = fx::Area::Other, .hours = 23.0f}).night ==
        0.0f);

  // Classic is untouched, option or not.
  fx::LookSettings classic;
  classic.day_night = true;
  const fx::FrameState c = fx::resolve_frame(classic, city_night);
  CHECK(c.grade.identity());
  CHECK(c.night == 0.0f);
  CHECK(c.weather == fx::Weather::None);
  CHECK(fx::resolve_frame(fx::LookSettings{}, city_night) ==
        fx::FrameState{.area = fx::Area::City});
}

TEST_CASE("the sun rises left and sets right", "[fx][daynight]") {
  const fx::Sun midnight = fx::sun_at(0.0f);
  CHECK(midnight.r == 0.0f);
  const fx::Sun morning = fx::sun_at(8.0f);
  const fx::Sun noon = fx::sun_at(12.0f);
  const fx::Sun evening = fx::sun_at(16.0f);
  CHECK(morning.x < 0.0f);
  CHECK(evening.x > 0.0f);
  CHECK(noon.z > morning.z);
  CHECK(noon.r == Catch::Approx(1.0f));
  CHECK(std::sqrt(noon.x * noon.x + noon.y * noon.y + noon.z * noon.z) ==
        Catch::Approx(1.0f));
}

TEST_CASE("street lamps line the kerbs and light up at night",
          "[fx][daynight][lighting]") {
  // A 16 x 10 map: a road along row 4 and one down column 7.
  auto is_road = [](int i, int j) { return j == 4 || i == 7; };
  const auto lamps = fx::place_street_lamps(16, 10, 8.0f, is_road, 4);
  REQUIRE_FALSE(lamps.empty());
  for (const fx::LampSpot& l : lamps) {
    const int i = static_cast<int>(l.x / 8.0f);
    const int j = static_cast<int>(l.y / 8.0f);
    CHECK(is_road(i, j));
    CHECK((i + 2 * j) % 4 == 0);
  }
  CHECK(lamps.size() == fx::place_street_lamps(16, 10, 8.0f, is_road).size());
  CHECK(fx::place_street_lamps(16, 10, 8.0f, is_road, 8).size() < lamps.size());
  CHECK(fx::place_street_lamps(16, 10, 8.0f, [](int, int) {
          return false;
        }).empty());

  CHECK_FALSE(fx::night_scene(0.0f, lamps).active());
  const lighting::Scene night = fx::night_scene(1.0f, lamps);
  CHECK(night.ambient == Catch::Approx(0.55f));
  CHECK(night.lights.size() == lamps.size());
  CHECK(night.ambient_b > night.ambient_r); // moonlight blue

  // Through the light pass: dark streets with pools around the lamps.
  ScopedHdScale scale(2);
  constexpr int w = 128;
  constexpr int h = 80;
  SoftGpu gpu(w * 2, h * 2);
  SDL_Texture* albedo =
      SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_Texture* out = SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_SetRenderTarget(gpu.renderer, albedo);
  SDL_SetRenderDrawColor(gpu.renderer, 200, 200, 200, 255);
  SDL_RenderClear(gpu.renderer);
  const std::vector<fx::LampSpot> one{{40.0f, 36.0f}};
  LightPass pass(gpu.renderer);
  pass.apply(albedo, out, w, h, 2, fx::night_scene(1.0f, one), 0.0);
  const auto px = gpu.read();
  const float at_lamp = red_of(px[static_cast<std::size_t>(72) * w * 2 + 80]);
  const float far = red_of(px[static_cast<std::size_t>(150) * w * 2 + 240]);
  CHECK(far < 200.0f * 0.5f);
  CHECK(at_lamp > far + 40.0f);
  SDL_DestroyTexture(out);
  SDL_DestroyTexture(albedo);
}

TEST_CASE("emissive maps glow where their art shows, and only there",
          "[fx][emissive][hd]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  ScopedMaps maps(dir.path());

  // A sign (20 x 20 texels) with a glowing mask, half covered by a wall
  // drawn after it.
  auto sign =
      std::make_shared<gfx::HdImage>(20, 20, gfx::pack_rgba(80, 80, 80, 255));
  gfx::HdImage mask(10, 10, gfx::pack_rgba(255, 200, 0, 255)); // half size
  REQUIRE(gfx::save_png(mask, dir.path() / "test" / "sign" / "emissive.png"));
  fx::MapStore::instance().bind(sign, "test/sign");
  auto wall =
      std::make_shared<gfx::HdImage>(10, 20, gfx::pack_rgba(30, 30, 30, 255));
  REQUIRE(fx::MapStore::instance().enabled());
  const auto loaded = fx::MapStore::instance().emissive(sign.get());
  REQUIRE(loaded);
  CHECK(loaded->width == 20); // resized to the picture
  CHECK(fx::MapStore::instance().emissive(wall.get()) == nullptr);

  gfx::Surface layer(30, 20, gfx::PaletteHandle{0}, 0);
  gfx::HdCmd a;
  a.img = sign;
  a.sw = 20;
  a.sh = 20;
  layer.hd_overlay(a, 0, 0, 60, 40);
  gfx::HdCmd b;
  b.img = wall;
  b.sw = 10;
  b.sh = 20;
  b.dx = 10;
  layer.hd_overlay(b, 0, 0, 60, 40);

  SoftGpu gpu(60, 40);
  HdCompositor comp(gpu.renderer);
  CHECK(comp.has_emissive(layer));
  SDL_Texture* glow = SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_TARGET, 60, 40);
  REQUIRE(comp.draw_emissive(glow, layer));
  const auto px = gpu.read();
  const uint32_t lit = px[10 * 60 + 5];
  CHECK(gfx::red(lit) >= 250);
  CHECK(gfx::green(lit) >= 195);
  CHECK(gfx::blue(lit) <= 5);
  CHECK(gfx::red(px[10 * 60 + 15]) == 0); // under the wall
  CHECK(gfx::red(px[10 * 60 + 40]) == 0); // no sign there
  // The 1994 side of the comparison does not glow.
  REQUIRE(comp.draw_emissive(glow, layer, 10));
  CHECK(gfx::red(gpu.read()[10 * 60 + 5]) == 0);
  SDL_DestroyTexture(glow);
}

TEST_CASE("without emissive maps nothing glows or changes",
          "[fx][emissive][hd]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  test_support::TempDir dir;
  ScopedMaps maps(dir.path()); // an empty maps folder
  auto sign =
      std::make_shared<gfx::HdImage>(20, 20, gfx::pack_rgba(80, 80, 80, 255));
  fx::MapStore::instance().bind(sign, "test/sign");
  gfx::Surface layer(30, 20, gfx::PaletteHandle{0}, 0);
  gfx::HdCmd a;
  a.img = sign;
  a.sw = 20;
  a.sh = 20;
  layer.hd_overlay(a, 0, 0, 60, 40);
  SoftGpu gpu(60, 40);
  HdCompositor comp(gpu.renderer);
  CHECK_FALSE(comp.has_emissive(layer));
  SDL_Texture* glow = SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_TARGET, 60, 40);
  SDL_SetRenderTarget(gpu.renderer, glow);
  SDL_SetRenderDrawColor(gpu.renderer, 9, 8, 7, 255);
  SDL_RenderClear(gpu.renderer);
  const auto before = gpu.read();
  CHECK_FALSE(comp.draw_emissive(glow, layer));
  SDL_SetRenderTarget(gpu.renderer, glow);
  CHECK(gpu.read() == before);
  SDL_DestroyTexture(glow);

  // No maps folder at all: lookups are null.
  fx::MapStore::instance().configure(dir.path() / "missing");
  CHECK_FALSE(fx::MapStore::instance().enabled());
  CHECK(fx::MapStore::instance().emissive(sign.get()) == nullptr);
  CHECK(fx::MapStore::instance().normal(sign.get()) == nullptr);
}

TEST_CASE("snow is bounded, its own, and falls", "[fx][weather]") {
  CHECK(fx::Snowfall::flake_count(fx::Weather::None, false, 1920, 1200) == 0);
  const std::size_t heavy =
      fx::Snowfall::flake_count(fx::Weather::Snow, false, 3840, 2400);
  const std::size_t light =
      fx::Snowfall::flake_count(fx::Weather::LightSnow, false, 3840, 2400);
  CHECK(heavy <= fx::Snowfall::max_flakes);
  CHECK(light < heavy);
  CHECK(fx::Snowfall::flake_count(fx::Weather::Snow, true, 3840, 2400) < heavy);

  const uint32_t game_rng = engine::random::get_state();
  fx::Snowfall a;
  fx::Snowfall b;
  a.configure(fx::Weather::Snow, false, 1280, 800);
  b.configure(fx::Weather::Snow, false, 1280, 800);
  REQUIRE(a.flakes().size() == heavy);
  for (int i = 0; i < 600; ++i) {
    a.step(1.0f / 60.0f);
    b.step(1.0f / 60.0f);
  }
  CHECK(engine::random::get_state() == game_rng); // never the game's numbers
  REQUIRE(a.flakes().size() == b.flakes().size());
  for (std::size_t i = 0; i < a.flakes().size(); ++i) {
    REQUIRE(a.flakes()[i].x == b.flakes()[i].x);
    REQUIRE(a.flakes()[i].y == b.flakes()[i].y);
  }
  for (const fx::Flake& f : a.flakes()) {
    CHECK(f.x >= 0.0f);
    CHECK(f.x <= 1280.0f);
    CHECK(f.y >= -f.size);
    CHECK(f.y <= 800.0f + f.size);
  }
  fx::Snowfall other(7);
  other.configure(fx::Weather::Snow, false, 1280, 800);
  fx::Snowfall same_seed;
  same_seed.configure(fx::Weather::Snow, false, 1280, 800);
  CHECK(other.flakes()[0].x != same_seed.flakes()[0].x);

  // A stall never makes the snow jump.
  fx::Snowfall c;
  c.configure(fx::Weather::Snow, false, 1280, 800);
  const fx::Flake before = c.flakes()[0];
  c.step(5.0f);
  const float moved = c.flakes()[0].y - before.y;
  CHECK((moved <= before.speed * 0.1f + 1e-3f || c.flakes()[0].y < 0.0f));

  // Reduced motion: slower, straight down.
  fx::Snowfall calm;
  calm.configure(fx::Weather::Snow, true, 1280, 800);
  float fastest = 0.0f;
  for (const fx::Flake& f : calm.flakes()) {
    CHECK(f.sway == 0.0f);
    fastest = std::max(fastest, f.speed);
  }
  float fastest_normal = 0.0f;
  for (const fx::Flake& f : a.flakes()) {
    fastest_normal = std::max(fastest_normal, f.speed);
  }
  CHECK(fastest < fastest_normal * 0.5f);

  // One batch: four corners and two triangles per flake.
  std::vector<SDL_Vertex> v;
  std::vector<int> idx;
  a.build(v, idx);
  CHECK(v.size() == a.flakes().size() * 4);
  CHECK(idx.size() == a.flakes().size() * 6);

  // Weather off empties the sky.
  a.configure(fx::Weather::None, false, 1280, 800);
  CHECK(a.flakes().empty());
}

TEST_CASE("snow draws over the frame in one call, and none draws nothing",
          "[fx][weather][hd]") {
  SoftGpu gpu(640, 400);
  SDL_SetRenderDrawColor(gpu.renderer, 20, 30, 60, 255);
  SDL_RenderClear(gpu.renderer);
  const auto before = gpu.read();
  fx::SnowLayer layer(gpu.renderer);
  layer.snowfall().configure(fx::Weather::None, false, 640, 400);
  CHECK_FALSE(layer.draw());
  CHECK(gpu.read() == before);
  layer.snowfall().configure(fx::Weather::Snow, false, 640, 400);
  CHECK(layer.draw());
  const auto after = gpu.read();
  int brighter = 0;
  for (std::size_t i = 0; i < after.size(); ++i) {
    brighter += gfx::blue(after[i]) > gfx::blue(before[i]) ? 1 : 0;
  }
  CHECK(brighter > 100);
  // Clipped to the HD side of the comparison.
  SDL_RenderClear(gpu.renderer);
  const SDL_Rect right{320, 0, 320, 400};
  layer.draw(&right);
  const auto clipped = gpu.read();
  for (int y = 0; y < 400; ++y) {
    for (int x = 0; x < 320; ++x) {
      REQUIRE(clipped[static_cast<std::size_t>(y) * 640 + x] ==
              before[static_cast<std::size_t>(y) * 640 + x]);
    }
  }
}

TEST_CASE("normal maps shape the light; flat maps change nothing",
          "[fx][normals]") {
  const fx::Normal flat = fx::decode_normal(gfx::pack_rgba(128, 128, 255, 255));
  CHECK(flat.x == 0.0f);
  CHECK(flat.y == 0.0f);
  CHECK(flat.z == 1.0f);
  const fx::Normal clear = fx::decode_normal(gfx::pack_rgba(0, 0, 0, 0));
  CHECK(clear.z == 1.0f);
  const fx::Normal up = fx::decode_normal(gfx::pack_rgba(128, 255, 128, 255));
  CHECK(up.y == Catch::Approx(1.0f));

  fx::NormalLights lights;
  lights.count = 1;
  lights.points[0] = {.x = -8.0f,
                      .y = 16.0f,
                      .height = 8.0f,
                      .radius = 80.0f,
                      .r = 1.0f,
                      .g = 0.9f,
                      .b = 0.7f};
  for (float x : {0.0f, 10.0f, 30.0f}) {
    const fx::Rgb f = fx::relief(lights, flat, x, 16.0f);
    CHECK(f.r == 1.0f);
    CHECK(f.g == 1.0f);
    CHECK(f.b == 1.0f);
  }

  // A bump lit from the left: its left side brighter than its right.
  const int size = 32;
  const gfx::HdImage normals = bump(size);
  const gfx::HdImage picture(size, size, gfx::pack_rgba(150, 150, 150, 255));
  const gfx::HdImage lit = fx::shade(lights, picture, normals, 0, 0);
  double left = 0;
  double right = 0;
  for (int y = 8; y < 24; ++y) {
    for (int x = 4; x < 12; ++x) {
      left += red_of(lit.at(x, y));
      right += red_of(lit.at(size - 1 - x, y));
    }
  }
  CHECK(left > right * 1.1);

  // A flat map, or no lights: the picture as it is.
  const gfx::HdImage flat_map(size, size, gfx::pack_rgba(128, 128, 255, 255));
  CHECK(fx::shade(lights, picture, flat_map, 0, 0).pixels == picture.pixels);
  const fx::NormalLights ambient_only;
  CHECK(ambient_only.empty());
  CHECK(fx::shade(ambient_only, picture, normals, 0, 0).pixels ==
        picture.pixels);

  // The sun from the left in the morning does the same.
  fx::NormalLights sunny;
  sunny.sun = fx::sun_at(8.0f);
  const gfx::HdImage morning = fx::shade(sunny, picture, normals, 0, 0);
  CHECK(red_of(morning.at(6, 16)) > red_of(morning.at(25, 16)));
}

TEST_CASE("normal-map lighting takes the scene's brightest lights",
          "[fx][normals]") {
  lighting::Scene scene;
  scene.ambient = 0.5f;
  for (int i = 0; i < 12; ++i) {
    lighting::Light l;
    l.x = static_cast<float>(i);
    l.y = 2.0f;
    l.radius = 10.0f + static_cast<float>(i);
    l.intensity = 0.5f + 0.04f * static_cast<float>(i);
    scene.lights.push_back(l);
  }
  const fx::NormalLights nl = fx::normal_lights(scene, 3, 0.0, fx::Sun{});
  CHECK(nl.count == fx::max_normal_lights);
  CHECK(nl.points[0].x == Catch::Approx(11.0f * 3.0f)); // the brightest
  CHECK(nl.points[0].radius == Catch::Approx(21.0f * 3.0f));
  CHECK(nl.ambient.r == Catch::Approx(0.5f));

  const fx::NormalLightUniforms u = fx::make_uniforms(nl);
  CHECK(u.count[0] == 8.0f);
  CHECK(u.light_pos[0][0] == nl.points[0].x);
  CHECK(u.light_pos[0][3] == nl.points[0].radius);
  CHECK(u.light_colour[7][0] == nl.points[7].r);
  CHECK(u.ambient[0] == nl.ambient.r);
}

TEST_CASE("normal-map lighting is a no-op off the GPU driver",
          "[fx][normals][hd]") {
  resources::load_fallback_palette();
  ScopedHdScale scale(2);
  SoftGpu gpu(60, 40);
  fx::NormalLightPass pass(gpu.renderer);
  CHECK_FALSE(pass.ready());
  auto img =
      std::make_shared<gfx::HdImage>(20, 20, gfx::pack_rgba(90, 120, 150, 255));
  gfx::Surface layer(30, 20, gfx::PaletteHandle{0}, 0);
  gfx::HdCmd a;
  a.img = img;
  a.sw = 20;
  a.sh = 20;
  layer.hd_overlay(a, 0, 0, 60, 40);
  HdCompositor comp(gpu.renderer);
  SDL_Texture* target = SDL_CreateTexture(
      gpu.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 60, 40);
  comp.draw_layer(target, layer, 1.0f);
  const auto plain = gpu.read();
  fx::NormalLights lights;
  lights.count = 1;
  lights.points[0] = {.x = 0, .y = 0, .height = 5, .radius = 50, .r = 1};
  comp.normal_lighting(&pass, &lights);
  comp.draw_layer(target, layer, 1.0f);
  CHECK(comp.normal_lit() == 0);
  CHECK(gpu.read() == plain);
  SDL_DestroyTexture(target);

  using namespace fx::shaders;
  CHECK(std::memcmp(normal_light_frag_dxil, "DXBC", 4) == 0);
  uint32_t magic = 0;
  std::memcpy(&magic, normal_light_frag_spirv, 4);
  CHECK(magic == 0x07230203u);
}

TEST_CASE("the phase 2 picture settings are saved and read back",
          "[fx][look]") {
  test_support::TempDir dir;
  const std::string file = (dir.path() / "settings.conf").string();
  GameOptions opts;
  opts.picture.day_night = true;
  opts.picture.reduced_motion = true;
  opts.picture.weather[fx::Area::NorthPole] = fx::Weather::Snow;
  opts.picture.weather[fx::Area::City] = fx::Weather::LightSnow;
  opts.picture.weather[fx::Area::Kitchen] = fx::Weather::None;
  {
    ConfigManager config(file);
    REQUIRE(config.save_options(opts));
  }
  ConfigManager reread(file);
  REQUIRE(reread.load_config());
  const GameOptions back = reread.load_options();
  CHECK(back.picture.day_night);
  CHECK(back.picture.reduced_motion);
  CHECK(back.picture.weather_for(fx::Area::NorthPole) == fx::Weather::Snow);
  CHECK(back.picture.weather_for(fx::Area::City) == fx::Weather::LightSnow);
  CHECK_FALSE(back.picture.weather.contains(fx::Area::Kitchen));
  CHECK(GameOptions{}.picture.weather_for(fx::Area::NorthPole) ==
        fx::Weather::None);
}
