/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The North Pole's ambient effects (fx/Ambient.h): only at the North Pole,
// never in a Classic city; Off is the same as no effects at all; the same
// game time draws the same frame; Reduced and Static have no twinkle; the
// music accent changes the glow's strength and nothing else.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <vector>

#include "TestSupport.h"
#include "engine/Renderable.h"
#include "engine/Renderer.h"
#include "engine/SDLContext.h"
#include "engine/SDL_include.h"
#include "engine/fx/Ambient.h"
#include "engine/fx/Look.h"
#include "engine/fx/Weather.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"
#include "engine/gfx/Surface.h"
#include "resources/PaletteLoader.h"

using test_support::ScopedHdScale;

namespace {

fx::FrameContext pole_city_night() {
  return {.area = fx::Area::City,
          .hours = 23.0f,
          .anim_seconds = 1234.5,
          .location = "north-pole",
          .map_rect = {40, 94, 320, 240}};
}

fx::LookSettings vivid(fx::AmbientMotion motion) {
  fx::LookSettings s;
  s.preset = fx::Preset::Vivid;
  s.ambient_motion = motion;
  return s;
}

// Restores the music level for the other tests.
struct MusicLevel {
  explicit MusicLevel(float level) { fx::set_music_level(level); }
  ~MusicLevel() { fx::set_music_level(0.0f); }
  MusicLevel(const MusicLevel&) = delete;
  MusicLevel& operator=(const MusicLevel&) = delete;
};

class FlatView : public Renderable {
public:
  void render(const RenderContext& ctx) override {
    if (!picture_) {
      picture_ = std::make_shared<const gfx::HdImage>(
          1280, 800, gfx::pack_rgba(20, 30, 60, 255));
    }
    gfx::HdCmd c;
    c.img = picture_;
    c.sw = 1280;
    c.sh = 800;
    ctx.surface->hd_overlay(c, 0, 0, 1280, 800);
  }
  bool is_dirty() const override { return dirty_; }
  void clean() override { dirty_ = false; }
  void mark_dirty() override { dirty_ = true; }
  bool needs_force_recompose() const override { return false; }
  void tick() override {}
  std::vector<LayerInfo> get_layers() override {
    return {LayerInfo{this, ViewRect{0, 0, 640, 400}, "map"}};
  }

private:
  std::shared_ptr<const gfx::HdImage> picture_;
  bool dirty_ = true;
};

// A fresh software Renderer at hd_scale 2, its frame as the window shows it.
std::vector<uint32_t> shown_frame(const fx::FrameState& state) {
  ScopedHdScale scale(2);
  resources::load_fallback_palette();
  SDL_Surface* canvas = SDL_CreateSurface(1280, 800, SDL_PIXELFORMAT_RGBA8888);
  std::vector<uint32_t> out;
  {
    SDLContext sdl;
    sdl.renderer.reset(SDL_CreateSoftwareRenderer(canvas));
    sdl.texture.reset(SDL_CreateTexture(sdl.renderer.get(),
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, 1280, 800));
    FlatView view;
    {
      Renderer renderer(sdl, 400);
      renderer.add_layers(&view);
      fx::FrameState graded = state;
      graded.graded_capture = true; // the aurora and snow are over the frame
      fx::set_frame_state(graded);
      renderer.render();
      renderer.with_current_frame([&out](SDL_Surface* s) {
        out.resize(static_cast<std::size_t>(s->w) * s->h);
        for (int y = 0; y < s->h; ++y) {
          std::memcpy(out.data() + static_cast<std::size_t>(y) * s->w,
                      static_cast<const uint8_t*>(s->pixels) + y * s->pitch,
                      static_cast<std::size_t>(s->w) * 4);
        }
      });
      fx::set_frame_state(fx::FrameState{});
    }
    sdl.texture.reset();
    sdl.renderer.reset();
  }
  SDL_DestroySurface(canvas);
  return out;
}

} // namespace

TEST_CASE("the ambient effects are the North Pole's alone", "[fx][ambient]") {
  const fx::FrameState full =
      fx::resolve_frame(vivid(fx::AmbientMotion::Full), pole_city_night());
  CHECK(full.aurora == 1.0f);
  CHECK(full.twinkle);
  CHECK(full.weather == fx::Weather::LightSnow); // its default snowfall
  CHECK(full.anim_seconds == 1234.5);

  // A Classic city (no location), or another location: none of it.
  fx::FrameContext classic_city = pole_city_night();
  classic_city.location.clear();
  const fx::FrameState none =
      fx::resolve_frame(vivid(fx::AmbientMotion::Full), classic_city);
  CHECK(none.aurora == 0.0f);
  CHECK_FALSE(none.twinkle);
  CHECK(none.weather == fx::Weather::None);
  CHECK(none.anim_seconds < 0.0);
  fx::FrameContext elsewhere = pole_city_night();
  elsewhere.location = "alps";
  CHECK(fx::resolve_frame(vivid(fx::AmbientMotion::Full), elsewhere) == none);

  // Off is exactly the frame state of no effects at all.
  CHECK(fx::resolve_frame(vivid(fx::AmbientMotion::Off), pole_city_night()) ==
        none);
  // A weather the player chose is kept.
  fx::LookSettings chosen = vivid(fx::AmbientMotion::Full);
  chosen.weather[fx::Area::City] = fx::Weather::Snow;
  CHECK(fx::resolve_frame(chosen, pole_city_night()).weather ==
        fx::Weather::Snow);
  // By day there is no aurora.
  fx::FrameContext day = pole_city_night();
  day.hours = 12.0f;
  CHECK(fx::resolve_frame(vivid(fx::AmbientMotion::Full), day).aurora == 0.0f);
}

TEST_CASE("Reduced and Static have no twinkle and less motion",
          "[fx][ambient]") {
  const auto reduced =
      fx::resolve_frame(vivid(fx::AmbientMotion::Reduced), pole_city_night());
  CHECK_FALSE(reduced.twinkle);
  CHECK(reduced.anim_seconds == 1234.5 * 0.25);
  CHECK(reduced.reduced_motion);
  const auto still =
      fx::resolve_frame(vivid(fx::AmbientMotion::Static), pole_city_night());
  CHECK_FALSE(still.twinkle);
  CHECK(still.anim_seconds == 0.0);
  CHECK(still.aurora == 1.0f); // drawn, but still
  // The reduced-motion option caps Full at Reduced.
  fx::LookSettings capped = vivid(fx::AmbientMotion::Full);
  capped.reduced_motion = true;
  CHECK(capped.effective_motion() == fx::AmbientMotion::Reduced);
  CHECK_FALSE(fx::resolve_frame(capped, pole_city_night()).twinkle);
}

TEST_CASE("the music accent changes the glow's strength and nothing else",
          "[fx][ambient]") {
  fx::FrameContext room = pole_city_night();
  room.area = fx::Area::Restaurant;
  fx::LookSettings s = vivid(fx::AmbientMotion::Full);
  s.preset = fx::Preset::Studio; // glow 0.7: room to grow
  const fx::FrameState quiet = fx::resolve_frame(s, room);
  s.music_accent = true;
  CHECK(fx::resolve_frame(s, room) == quiet); // level 0: no change
  MusicLevel loud(0.8f);
  fx::FrameState pulsed = fx::resolve_frame(s, room);
  CHECK(pulsed.emissive > quiet.emissive);
  CHECK(pulsed.emissive <= 1.0f);
  pulsed.emissive = quiet.emissive;
  CHECK(pulsed == quiet);
  // Off, or below Full, the music is not followed.
  s.music_accent = false;
  CHECK(fx::resolve_frame(s, room) == quiet);
  s.music_accent = true;
  s.ambient_motion = fx::AmbientMotion::Reduced;
  CHECK(fx::resolve_frame(s, room).emissive == quiet.emissive);
}

TEST_CASE("twinkle and aurora are pure functions of place and time",
          "[fx][ambient]") {
  bool varies_in_space = false;
  bool varies_in_time = false;
  for (int i = 0; i < 64; ++i) {
    const float f = fx::twinkle(i, 3, 10.0);
    CHECK(f >= 1.0f - fx::twinkle_depth);
    CHECK(f <= 1.0f + fx::twinkle_depth);
    CHECK(f == fx::twinkle(i, 3, 10.0));
    varies_in_space |= f != fx::twinkle(0, 3, 10.0);
    varies_in_time |= f != fx::twinkle(i, 3, 17.3);
  }
  CHECK(varies_in_space);
  CHECK(varies_in_time);

  std::vector<SDL_Vertex> v;
  std::vector<int> idx;
  fx::twinkle_mesh({0, 0, 160, 80}, 1.0f, 3.0, 16.0f, v, idx);
  CHECK(v.size() == 11u * 6u);
  CHECK(idx.size() == 10u * 5u * 6u);
  for (const SDL_Vertex& x : v) {
    CHECK(x.color.r <= 1.0f); // clamped
  }

  const SDL_FRect rect{80, 188, 640, 480};
  fx::aurora_mesh(rect, 50.0, 0.0f, v, idx);
  CHECK(v.empty());
  fx::aurora_mesh(rect, 50.0, 1.0f, v, idx);
  REQUIRE_FALSE(v.empty());
  std::vector<SDL_Vertex> again;
  std::vector<int> again_idx;
  fx::aurora_mesh(rect, 50.0, 1.0f, again, again_idx);
  for (std::size_t i = 0; i < v.size(); ++i) {
    REQUIRE(v[i].position.x == again[i].position.x);
    REQUIRE(v[i].position.y == again[i].position.y);
    CHECK(v[i].position.x >= rect.x);
    CHECK(v[i].position.x <= rect.x + rect.w);
    CHECK(v[i].color.a <= 0.22f);
  }
}

TEST_CASE("snow is a pure function of the game time", "[fx][ambient]") {
  fx::Snowfall a;
  a.configure(fx::Weather::Snow, false, 1280, 800);
  a.set_time(42.0);
  const auto at42 = a.flakes();
  a.set_time(900.0);
  a.set_time(42.0);
  for (std::size_t i = 0; i < at42.size(); ++i) {
    REQUIRE(a.flakes()[i].x == at42[i].x);
    REQUIRE(a.flakes()[i].y == at42[i].y);
  }
  fx::Snowfall b;
  b.configure(fx::Weather::Snow, false, 1280, 800);
  b.set_time(42.0);
  CHECK(b.flakes()[7].y == at42[7].y);
  for (const fx::Flake& f : a.flakes()) {
    CHECK(f.x >= 0.0f);
    CHECK(f.x <= 1280.0f);
    CHECK(f.y >= -f.size);
    CHECK(f.y <= 800.0f + f.size);
  }
}

TEST_CASE("the same game time draws the same frame", "[fx][ambient][hd]") {
  const fx::FrameState plain;
  const fx::FrameState effects =
      fx::resolve_frame(vivid(fx::AmbientMotion::Full), pole_city_night());
  REQUIRE(effects.aurora > 0.0f);
  const auto a = shown_frame(effects);
  const auto b = shown_frame(effects);
  CHECK(a == b);
  CHECK(a != shown_frame(plain)); // the aurora and snow are there
  fx::FrameState later = effects;
  later.anim_seconds += 7.0;
  CHECK(shown_frame(later) != a);
  // Off draws exactly what no effects draw.
  const fx::FrameState off =
      fx::resolve_frame(vivid(fx::AmbientMotion::Off), pole_city_night());
  fx::FrameContext classic_city = pole_city_night();
  classic_city.location.clear();
  CHECK(shown_frame(off) == shown_frame(fx::resolve_frame(
                                vivid(fx::AmbientMotion::Full), classic_city)));
}
