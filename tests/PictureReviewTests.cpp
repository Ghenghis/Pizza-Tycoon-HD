/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Review captures: the `picture` command's settings keys match the settings
// file's, and graded screenshots show the frame as the window does, while
// the default (ungraded) screenshots stay the frame itself, byte for byte.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/state/GameOptions.h"
#include "core/util/ConfigManager.h"
#include "engine/Renderable.h"
#include "engine/Renderer.h"
#include "engine/SDLContext.h"
#include "engine/SDL_include.h"
#include "engine/fx/Look.h"
#include "engine/gfx/HdList.h"
#include "resources/PaletteLoader.h"

using test_support::ScopedHdScale;

namespace {

// One full-screen layer: an HD gradient (warm left, cool right).
class GradientView : public Renderable {
public:
  void render(const RenderContext& ctx) override {
    const int k = gfx::hd_scale();
    auto img = std::make_shared<gfx::HdImage>(640 * k, 400 * k, 0u);
    for (int y = 0; y < img->height; ++y) {
      for (int x = 0; x < img->width; ++x) {
        const auto t = static_cast<uint32_t>(x * 200 / img->width);
        img->at(x, y) = gfx::pack_rgba(40 + t, 90, 230 - t, 255);
      }
    }
    gfx::HdCmd c;
    c.img = img;
    c.sw = img->width;
    c.sh = img->height;
    ctx.surface->hd_overlay(c, 0, 0, img->width, img->height);
  }
  bool is_dirty() const override { return dirty_; }
  void clean() override { dirty_ = false; }
  void mark_dirty() override { dirty_ = true; }
  bool needs_force_recompose() const override { return false; }
  void tick() override {}
  std::vector<LayerInfo> get_layers() override {
    return {LayerInfo{this, ViewRect{0, 0, 640, 400}, "gradient"}};
  }

private:
  bool dirty_ = true;
};

// A software-rendered Renderer at hd_scale 2 (1280 x 800).
struct SoftScreen {
  ScopedHdScale scale{2};
  SDL_Surface* canvas = SDL_CreateSurface(1280, 800, SDL_PIXELFORMAT_RGBA8888);
  SDLContext sdl;
  GradientView view;
  std::unique_ptr<Renderer> renderer;

  SoftScreen() {
    resources::load_fallback_palette();
    sdl.renderer.reset(SDL_CreateSoftwareRenderer(canvas));
    REQUIRE(sdl.renderer);
    sdl.texture.reset(SDL_CreateTexture(sdl.renderer.get(),
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, 1280, 800));
    REQUIRE(sdl.texture);
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

  std::vector<uint32_t> capture(const fx::FrameState& state) {
    fx::set_frame_state(state);
    renderer->render();
    std::vector<uint32_t> out;
    renderer->with_current_frame([&out](SDL_Surface* s) {
      REQUIRE(s->format == SDL_PIXELFORMAT_RGBA8888);
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

double ratio_rb(const std::vector<uint32_t>& px) {
  double r = 0;
  double b = 0;
  for (uint32_t p : px) {
    r += gfx::red(p);
    b += gfx::blue(p);
  }
  return r / b;
}

} // namespace

TEST_CASE("the picture keys are the settings file's keys", "[fx][review]") {
  // Every key the command knows round-trips through settings.conf to the
  // same settings.
  fx::LookSettings set;
  const std::vector<std::pair<std::string, std::string>> values{
      {"picture_preset", "vivid"},
      {"picture_intensity", "10"},
      {"picture_shadows", "-20"},
      {"picture_bloom", "35"},
      {"picture_warmth", "-15"},
      {"picture_vignette", "5"},
      {"picture_crt", "on"},
      {"picture_city", "cinematic"},
      {"picture_restaurant", "studio"},
      {"picture_kitchen", "same"},
      {"picture_north_pole", "natural"},
      {"picture_day_night", "true"},
      {"picture_reduced_motion", "1"},
      {"weather_city", "light_snow"},
      {"weather_restaurant", "none"},
      {"weather_kitchen", "snow"},
      {"weather_north_pole", "snow"},
      {"picture_screenshots", "on"},
  };
  REQUIRE(values.size() == fx::setting_keys().size());
  for (const auto& [key, value] : values) {
    INFO(key);
    CHECK_FALSE(fx::apply_setting(set, key, value).has_value());
  }
  CHECK(set.graded_screenshots);
  CHECK(set.sliders.crt);
  CHECK(set.preset_for(fx::Area::City) == fx::Preset::Cinematic);
  CHECK(set.weather_for(fx::Area::Kitchen) == fx::Weather::Snow);

  test_support::TempDir dir;
  const std::string file = (dir.path() / "settings.conf").string();
  GameOptions opts;
  opts.picture = set;
  {
    ConfigManager config(file);
    REQUIRE(config.save_options(opts));
  }
  ConfigManager reread(file);
  REQUIRE(reread.load_config());
  const fx::LookSettings back = reread.load_options().picture;
  for (std::string_view key : fx::setting_keys()) {
    INFO(std::string(key));
    CHECK(fx::setting_value(back, key) == fx::setting_value(set, key));
  }
  // And the settings file lists every one of them.
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  for (std::string_view key : fx::setting_keys()) {
    INFO(std::string(key));
    CHECK(text.str().find("#" + std::string(key) + "=") != std::string::npos);
  }

  CHECK(fx::apply_setting(set, "picture_sepia", "on").has_value());
  CHECK(fx::apply_setting(set, "picture_bloom", "51").has_value());
  CHECK(fx::apply_setting(set, "picture_bloom", "lots").has_value());
  CHECK(fx::apply_setting(set, "picture_crt", "maybe").has_value());
  CHECK(fx::apply_setting(set, "weather_city", "rain").has_value());
  CHECK(fx::setting_value(set, "picture_sepia").empty());
}

TEST_CASE("screenshots stay the frame itself unless asked to be graded",
          "[fx][review][hd]") {
  SoftScreen screen;
  const std::vector<uint32_t> plain = screen.capture(fx::FrameState{});
  REQUIRE(plain.size() == std::size_t{1280} * 800);

  fx::FrameState warm;
  warm.grade.warmth = 0.6f;
  warm.weather = fx::Weather::Snow;
  // Not graded (the default): the frame underneath, untouched by the grade
  // and the snow.
  screen.renderer->redraw_all();
  CHECK(screen.capture(warm) == plain);

  // Graded, Classic and clear: byte for byte the same.
  fx::FrameState classic;
  classic.graded_capture = true;
  screen.renderer->redraw_all();
  CHECK(screen.capture(classic) == plain);

  // Graded with warmth: what the window shows (fixed-function here).
  fx::FrameState graded_warm;
  graded_warm.grade.warmth = 0.6f;
  graded_warm.graded_capture = true;
  screen.renderer->redraw_all();
  const auto w = screen.capture(graded_warm);
  CHECK(ratio_rb(w) > ratio_rb(plain) * 1.05);

  // Graded with snow: the flakes are in the picture.
  fx::FrameState snowy;
  snowy.weather = fx::Weather::Snow;
  snowy.graded_capture = true;
  screen.renderer->redraw_all();
  const auto s = screen.capture(snowy);
  int changed = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    changed += s[i] != plain[i] ? 1 : 0;
  }
  CHECK(changed > 500);
}
