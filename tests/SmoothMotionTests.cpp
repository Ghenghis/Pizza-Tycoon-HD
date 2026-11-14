/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Smooth motion is drawing only: things are drawn between their last two
// simulation positions (tween::drawn), the city map camera glides
// (CameraSmoother) and zooms ease between scales (ZoomTransition). These
// tests cover the maths, teleport snapping, that alpha 0 and 1 draw exactly
// the previous and current positions, and that the game state is the same
// with the option on and off.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ContentTestGame.h"
#include "core/city/Car.h"
#include "core/city/City.h"
#include "core/common/Ids.h"
#include "core/init/NewGameConfig.h"
#include "core/save/SaveGameManager.h"
#include "core/state/GameOptions.h"
#include "core/state/GameState.h"
#include "core/util/Constants.h"
#include "core/util/Tween.h"
#include "engine/Random.h"
#include "engine/geom/Camera.h"
#include "engine/geom/Spaces.h"
#include "engine/gfx/Surface.h"
#include "views/helpers/ZoomTransition.h"
#include "views/helpers/camera/CameraSmoother.h"

namespace fs = std::filesystem;
using Catch::Approx;

TEST_CASE("Smooth motion: interpolation ends exactly on both positions",
          "[smooth][motion]") {
  CHECK(tween::lerp(10, 20, 0.0f) == 10);
  CHECK(tween::lerp(10, 20, 1.0f) == 20);
  CHECK(tween::lerp(10, 20, 0.5f) == 15);
  CHECK(tween::lerp(10, 20, 0.26f) == 13); // rounded to the nearest pixel
  CHECK(tween::lerp(20, 10, 0.26f) == 17);
  // Outside 0..1 it stays on the ends.
  CHECK(tween::lerp(10, 20, -0.5f) == 10);
  CHECK(tween::lerp(10, 20, 1.5f) == 20);

  const WorldPoint prev{100, 50};
  const WorldPoint curr{104, 48};
  CHECK(tween::drawn(prev, curr, 0.0f, 4) == prev);
  CHECK(tween::drawn(prev, curr, 1.0f, 4) == curr);
  CHECK(tween::drawn(prev, curr, 0.5f, 4) == WorldPoint{102, 49});
  // Every alpha lies on the way.
  for (int i = 0; i <= 100; ++i) {
    const WorldPoint p =
        tween::drawn(prev, curr, static_cast<float>(i) / 100.0f, 4);
    CHECK(p.x >= 100);
    CHECK(p.x <= 104);
    CHECK(p.y >= 48);
    CHECK(p.y <= 50);
  }
}

TEST_CASE("Smooth motion: a teleport is drawn at once", "[smooth][motion]") {
  const WorldPoint prev{0, 0};
  CHECK_FALSE(tween::is_teleport(prev, WorldPoint{4, -4}, 4));
  CHECK(tween::is_teleport(prev, WorldPoint{5, 0}, 4));
  CHECK(tween::is_teleport(prev, WorldPoint{0, -5}, 4));
  for (const float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
    CHECK(tween::drawn(prev, WorldPoint{32, 0}, alpha, 4) == WorldPoint{32, 0});
  }
}

TEST_CASE("Smooth motion: easing and the damped spring", "[smooth][motion]") {
  CHECK(tween::ease_out_cubic(0.0f) == 0.0f);
  CHECK(tween::ease_out_cubic(1.0f) == 1.0f);
  CHECK(tween::ease_out_cubic(0.5f) == Approx(0.875f));
  float last = 0.0f;
  for (int i = 1; i <= 20; ++i) {
    const float e = tween::ease_out_cubic(static_cast<float>(i) / 20.0f);
    CHECK(e >= last);
    last = e;
  }

  // Critically damped: it reaches the target without overshooting and
  // settles, whatever the frame time.
  for (const float dt : {4.0f, 16.0f, 33.0f, 50.0f}) {
    INFO("frame " << dt << " ms");
    tween::Spring s;
    s.snap(0.0f);
    float t = 0.0f;
    while (t < 1000.0f && !s.settled(100.0f)) {
      s.follow(100.0f, dt, 45.0f);
      CHECK(s.position <= 100.0f + 0.001f);
      t += dt;
    }
    CHECK(s.settled(100.0f));
    CHECK(t < 600.0f);
  }
}

namespace {

Camera test_camera(int zoom = 1) {
  Camera c;
  c.view_size(Size{320, 200});
  c.world_size(Size{5120, 3840});
  c.zoom(zoom);
  c.scroll(WorldPoint{640, 480});
  return c;
}

} // namespace

TEST_CASE("Smooth motion: the drawn camera is the camera at alpha 0 and 1",
          "[smooth][motion][camera]") {
  Camera camera = test_camera();
  CameraSmoother smoother;
  // Before it has seen a tick: the camera as it is.
  CHECK(smoother.drawn(camera, 0.3f) == camera.scroll());

  smoother.on_tick(camera);
  const WorldPoint before = camera.scroll();
  camera.scroll(WorldPoint{before.x + 16, before.y - 8});
  smoother.on_tick(camera);
  CHECK(smoother.drawn(camera, 0.0f) == before);
  CHECK(smoother.drawn(camera, 1.0f) == camera.scroll());
  CHECK(smoother.drawn(camera, 0.5f) == WorldPoint{before.x + 8, before.y - 4});
  CHECK(smoother.moving());

  // A tick without movement ends the motion.
  smoother.on_tick(camera);
  CHECK(smoother.drawn(camera, 0.0f) == camera.scroll());
  CHECK_FALSE(smoother.moving());

  // Further than half a view in one tick is a jump: drawn at once.
  camera.scroll(WorldPoint{camera.scroll().x + 1000, camera.scroll().y});
  smoother.on_tick(camera);
  CHECK(smoother.drawn(camera, 0.0f) == camera.scroll());

  // A new zoom is drawn at once too.
  camera.scroll(WorldPoint{camera.scroll().x + 8, camera.scroll().y});
  smoother.on_tick(camera);
  camera.zoom(4);
  CHECK(smoother.drawn(camera, 0.0f) == camera.scroll());
}

TEST_CASE("Smooth motion: a drag release settles on the camera with a spring",
          "[smooth][motion][camera]") {
  Camera camera = test_camera();
  CameraSmoother smoother;
  smoother.on_tick(camera);
  smoother.advance(camera, 16, 0.0f, false);

  // A drag moves the camera between ticks; the picture follows behind.
  const WorldPoint start = camera.scroll();
  camera.scroll(WorldPoint{start.x + 60, start.y});
  smoother.advance(camera, 16, 0.0f, true);
  const WorldPoint during = smoother.drawn(camera, 0.0f);
  CHECK(during.x > start.x);
  CHECK(during.x < start.x + 60);

  // Let go: it glides on into place, without passing it, and then draws the
  // camera exactly again.
  smoother.on_tick(camera);
  smoother.on_tick(camera);
  int frames = 0;
  int last_x = during.x;
  while (smoother.moving() && frames < 200) {
    smoother.advance(camera, 16, 0.0f, false);
    const WorldPoint p = smoother.drawn(camera, 0.0f);
    CHECK(p.x >= last_x);
    CHECK(p.x <= camera.scroll().x);
    last_x = p.x;
    ++frames;
  }
  CHECK_FALSE(smoother.moving());
  CHECK(frames > 1);
  CHECK(frames < 60); // about a second at most
  CHECK(smoother.drawn(camera, 0.0f) == camera.scroll());
  CHECK(smoother.drawn(camera, 0.7f) == camera.scroll());
}

TEST_CASE("Smooth motion: the zoom transition starts on the old frame and "
          "ends on the new one",
          "[smooth][motion][zoom]") {
  using views::helpers::MapFrame;
  using views::helpers::ZoomTransition;
  // The old frame shows world 0..64 at 4 world pixels per screen pixel; the
  // new one world 24..40 at 1, zoomed in on (32, 32).
  const MapFrame from{.viewport = ViewRect{0, 0, 16, 16},
                      .top_left = WorldPoint{0, 0},
                      .zoom = 4};
  const MapFrame to{.viewport = ViewRect{0, 0, 16, 16},
                    .top_left = WorldPoint{24, 24},
                    .zoom = 1};
  const WorldPoint anchor{32, 32};

  ZoomTransition z;
  z.begin(from, std::vector<uint8_t>(16 * 16, 7), to, anchor);
  REQUIRE(z.active());
  CHECK(z.eased() == 0.0f);
  // At the start each screen pixel shows what the old frame showed there,
  // at the end what the new one does.
  for (const int p : {0, 5, 15}) {
    const float c = static_cast<float>(p) + 0.5f;
    CHECK(from.screen_x(z.world_x(c, 0.0f)) == Approx(c));
    CHECK(from.screen_y(z.world_y(c, 0.0f)) == Approx(c));
    CHECK(to.screen_x(z.world_x(c, 1.0f)) == Approx(c));
    CHECK(to.screen_y(z.world_y(c, 1.0f)) == Approx(c));
  }
  // The anchor goes from where it was to where it is now.
  CHECK(z.world_x(from.screen_x(32.0f), 0.0f) == Approx(32.0f));
  CHECK(z.world_x(to.screen_x(32.0f), 1.0f) == Approx(32.0f));

  // The dissolve hands over evenly.
  int shown = 0;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      shown += ZoomTransition::shows_new(ViewPoint{x, y}, 0.5f) ? 1 : 0;
      CHECK_FALSE(ZoomTransition::shows_new(ViewPoint{x, y}, 0.0f));
      CHECK(ZoomTransition::shows_new(ViewPoint{x, y}, 1.0f));
    }
  }
  CHECK(shown == 8);

  gfx::Surface surface(16, 16, std::nullopt, 9);
  z.composite(surface);
  for (const uint8_t px : surface.pixels()) {
    CHECK(px == 7); // all the old frame
  }

  z.advance(ZoomTransition::duration_ms - 1);
  REQUIRE(z.active());
  gfx::Surface late(16, 16, std::nullopt, 9);
  z.composite(late);
  for (const uint8_t px : late.pixels()) {
    CHECK(px == 9); // all the new frame
  }

  z.advance(1);
  CHECK_FALSE(z.active());
  gfx::Surface after(16, 16, std::nullopt, 9);
  after.put(ViewPoint{3, 3}, 1);
  z.composite(after); // nothing more to draw
  CHECK(after.get(ViewPoint{3, 3}) == 1);
  CHECK(after.get(ViewPoint{4, 4}) == 9);
}

TEST_CASE("Smooth motion: a city car is drawn on its way, and snaps on "
          "turns and jumps",
          "[smooth][motion][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  content_test::start_classic_game(game, 0x5300, GameMode::FREEMODE, 0);
  const City& city = *game.state.city(core::CityId{0});

  // A road tile a car can start on, well inside the map.
  std::optional<Car> car;
  for (int ty = 20; ty < MAP_TILES_PER_COLUMN - 20 && !car; ++ty) {
    for (int tx = 20; tx < MAP_TILES_PER_ROW - 20 && !car; ++tx) {
      if (Car::spawn_mask(city.type((ty * MAP_TILES_PER_ROW) + tx)) != 0) {
        car = Car::spawn(tx, ty, 0, city);
      }
    }
  }
  REQUIRE(car.has_value());
  CHECK(car->drawn_position(0.0f) == car->position());

  // The whole map is in view, so the car never wraps to the other lane.
  const WorldRect view{0, 0, MAP_TILES_PER_ROW * tile_width_pixels,
                       MAP_TILES_PER_COLUMN * tile_width_pixels};
  int glided = 0;
  int snapped = 0;
  for (int update = 0; update < 400; ++update) {
    const Dir dir_before = car->dir();
    car->update(view, city);
    INFO("update " << update);
    // Alpha 1 is always where the car is.
    CHECK(car->drawn_position(1.0f) == car->position());
    if (car->previous_position() == car->position()) {
      CHECK(car->drawn_position(0.0f) == car->position());
      continue;
    }
    const bool teleport =
        car->dir() != dir_before ||
        tween::is_teleport(car->previous_position(), car->position(),
                           Car::max_drawn_step_pixels);
    if (teleport) {
      // Turns and jumps are drawn at once, as the original does.
      CHECK(car->drawn_position(0.0f) == car->position());
      CHECK(car->drawn_position(0.5f) == car->position());
      ++snapped;
    }
    else {
      // Alpha 0 is where the car was.
      CHECK(car->drawn_position(0.0f) == car->previous_position());
      const WorldPoint half = car->drawn_position(0.5f);
      CHECK(std::abs(half.x - car->previous_position().x) <=
            Car::max_drawn_step_pixels);
      CHECK(std::abs(half.y - car->previous_position().y) <=
            Car::max_drawn_step_pixels);
      ++glided;
    }
  }
  CHECK(glided > 100);
  INFO("turns and jumps " << snapped);
}

namespace {

std::string platform_id() {
  std::string os = "other";
#if defined(_WIN32)
  os = "windows";
#elif defined(__ANDROID__)
  os = "android";
#elif defined(__APPLE__)
  os = "macos";
#elif defined(__linux__)
  os = "linux";
#endif
  std::string compiler = "other";
#if defined(_MSC_VER) && !defined(__clang__)
  compiler = "msvc";
#elif defined(__clang__)
  compiler = "clang";
#elif defined(__GNUC__)
  compiler = "gcc";
#endif
  std::string arch = "other";
#if defined(_M_X64) || defined(__x86_64__)
  arch = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  arch = "arm64";
#endif
  return os + "-" + compiler + "-" + arch;
}

// The Classic golden quick start in Madrid (ClassicFingerprintTests.cpp),
// with smooth motion as given; its fingerprints at start and on day 4.
std::vector<std::string> quick_start_madrid(bool smooth_motion) {
  content_test::TestGame game;
  engine::random::seed(0x50495A5Au);
  auto& state = game.state;
  state.options().autosave_frequency = AutosaveFrequency::Off;
  state.options().rules.fix_legacy_bugs = true;
  state.options().smooth_motion = smooth_motion;
  state.begin_initialization();
  auto& config = state.initializer();
  config.mode = GameMode::QUICKSTART;
  config.size = init::RestaurantSize::BIG;
  config.difficulty = init::DifficultyLevel::PUSHOVER;
  config.city_id = core::CityId{6};
  content_test::add_players(config);
  state.finalize_initialization(game.context);
  REQUIRE(state.active_player() != nullptr);
  std::vector<std::string> prints;
  prints.push_back(
      save::SaveGameManager::fingerprint(state, game.context).hash);
  content_test::play_days(state, 3);
  prints.push_back(
      save::SaveGameManager::fingerprint(state, game.context).hash);
  return prints;
}

} // namespace

TEST_CASE("Smooth motion: the game state is the same with it on and off",
          "[smooth][motion][golden][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  const auto on = quick_start_madrid(true);
  const auto off = quick_start_madrid(false);
  CHECK(on == off);

  // And it is the Classic golden value where one is recorded for this
  // platform (the golden file is only read here).
  const fs::path golden = fs::path(__FILE__).parent_path() / "fixtures" /
                          "classic" / "golden-fingerprints.json";
  std::ifstream in(golden);
  REQUIRE(in.good());
  const auto doc = nlohmann::json::parse(in);
  const std::string platform = platform_id();
  const auto expected = [&](const char* name) -> std::optional<std::string> {
    const auto& checkpoints = doc.at("checkpoints");
    if (!checkpoints.contains(name) ||
        !checkpoints.at(name).contains(platform)) {
      return std::nullopt;
    }
    return checkpoints.at(name)
        .at(platform)
        .at("fingerprint")
        .get<std::string>();
  };
  if (const auto start = expected("quick-start-madrid/started")) {
    CHECK(on[0] == *start);
  }
  if (const auto day4 = expected("quick-start-madrid/day-4")) {
    CHECK(on[1] == *day4);
  }
}
