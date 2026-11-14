/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The picture settings (engine/fx): Classic is the identity, byte for byte;
// every preset and slider moves the picture the way it says, checked on the
// CPU reference of the post-FX shader; and on the software renderer the
// final draw and the light pass are unchanged at the identity.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/state/GameOptions.h"
#include "core/util/ConfigManager.h"
#include "engine/LightPass.h"
#include "engine/Lighting.h"
#include "engine/SDL_include.h"
#include "engine/fx/Grade.h"
#include "engine/fx/Look.h"
#include "engine/fx/PostFx.h"
#include "engine/fx/generated/PostFxShaders.h"

namespace {

constexpr int W = 96;
constexpr int H = 60;

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
  return (r << 24) | (g << 16) | (b << 8) | a;
}
float red(uint32_t p) { return static_cast<float>((p >> 24) & 0xFF); }
float green(uint32_t p) { return static_cast<float>((p >> 16) & 0xFF); }
float blue(uint32_t p) { return static_cast<float>((p >> 8) & 0xFF); }
float luma(uint32_t p) {
  return 0.2126f * red(p) + 0.7152f * green(p) + 0.0722f * blue(p);
}

// A test picture: a grey ramp across, colour bands down, dark corners
// excluded (mid tones everywhere, so no channel starts clipped), and one
// small bright lamp in the middle.
std::vector<uint32_t> test_image() {
  std::vector<uint32_t> px(static_cast<std::size_t>(W) * H);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const auto ramp = static_cast<uint32_t>(40 + x * 150 / W);
      uint32_t r = ramp;
      uint32_t g = ramp;
      uint32_t b = ramp;
      switch ((y / 10) % 3) {
      case 0:
        r = std::min(255u, r + 40);
        break;
      case 1:
        g = std::min(255u, g + 40);
        break;
      default:
        b = std::min(255u, b + 40);
        break;
      }
      px[static_cast<std::size_t>(y) * W + x] = rgba(r, g, b);
    }
  }
  for (int y = H / 2 - 2; y < H / 2 + 2; ++y) {
    for (int x = W / 2 - 2; x < W / 2 + 2; ++x) {
      px[static_cast<std::size_t>(y) * W + x] = rgba(255, 250, 235);
    }
  }
  return px;
}

std::vector<uint32_t> graded(const fx::Grade& g,
                             const std::vector<uint32_t>& in,
                             fx::FrameInfo info = {}) {
  std::vector<uint32_t> out(in.size());
  info.mask_cols = W;
  fx::grade_frame(g, in.data(), out.data(), W, H, info);
  return out;
}

struct Stats {
  double r = 0, g = 0, b = 0, luma = 0, luma_sd = 0, chroma = 0;
};

Stats stats(const std::vector<uint32_t>& px) {
  Stats s;
  for (uint32_t p : px) {
    s.r += red(p);
    s.g += green(p);
    s.b += blue(p);
    s.luma += luma(p);
    s.chroma += std::max({red(p), green(p), blue(p)}) -
                std::min({red(p), green(p), blue(p)});
  }
  const auto n = static_cast<double>(px.size());
  s.r /= n;
  s.g /= n;
  s.b /= n;
  s.luma /= n;
  s.chroma /= n;
  for (uint32_t p : px) {
    s.luma_sd += (luma(p) - s.luma) * (luma(p) - s.luma);
  }
  s.luma_sd = std::sqrt(s.luma_sd / n);
  return s;
}

uint32_t at(const std::vector<uint32_t>& px, int x, int y) {
  return px[static_cast<std::size_t>(y) * W + x];
}

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
  // The canvas (or the current target) as RGBA8888 texels.
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

SDL_Texture* upload(SDL_Renderer* r, std::vector<uint32_t>& px) {
  SDL_Surface* s =
      SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_RGBA8888, px.data(), W * 4);
  REQUIRE(s);
  SDL_Texture* tex = SDL_CreateTextureFromSurface(r, s);
  SDL_DestroySurface(s);
  REQUIRE(tex);
  // Copied as it is: the comparisons below are byte for byte.
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
  return tex;
}

} // namespace

TEST_CASE("Classic is the identity", "[fx][look]") {
  CHECK(fx::preset_grade(fx::Preset::Classic).identity());
  const fx::LookSettings defaults;
  CHECK(defaults.preset == fx::Preset::Classic);
  for (fx::Area a : {fx::Area::Other, fx::Area::City, fx::Area::Restaurant,
                     fx::Area::Kitchen, fx::Area::NorthPole}) {
    CHECK(fx::resolve(defaults, a).identity());
  }
  CHECK(fx::Grade{}.balance() == std::array<float, 3>{1.0f, 1.0f, 1.0f});
  // Every other preset changes something.
  for (fx::Preset p : fx::all_presets) {
    if (p != fx::Preset::Classic) {
      CHECK_FALSE(fx::preset_grade(p).identity());
    }
  }
  // Sliders at a limit Classic cannot go past stay the identity.
  fx::LookSettings clamped;
  clamped.sliders.bloom = fx::Sliders::MIN;
  clamped.sliders.vignette = fx::Sliders::MIN;
  CHECK(fx::resolve(clamped, fx::Area::Other).identity());
}

TEST_CASE("the identity grade copies the frame byte for byte", "[fx][look]") {
  std::mt19937 rng(7);
  std::uniform_int_distribution<uint32_t> any;
  std::vector<uint32_t> in(static_cast<std::size_t>(W) * H);
  for (uint32_t& p : in) {
    p = any(rng);
  }
  const auto out = graded(fx::Grade{}, in);
  CHECK(std::memcmp(in.data(), out.data(), in.size() * 4) == 0);
}

TEST_CASE("the per-texel maths at neutral settings returns the texel",
          "[fx][look]") {
  // Even without the shortcut of grade_frame, the neutral grade leaves a
  // texel where it was (to float precision), so the shader agrees.
  const fx::Grade g;
  for (float v : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
    const fx::Rgb c = fx::grade_texel(g, {v, v * 0.5f, 1.0f - v}, {}, 0.3f,
                                      0.7f, fx::FrameInfo{});
    CHECK(c.r == Catch::Approx(v).margin(1e-6));
    CHECK(c.g == Catch::Approx(v * 0.5f).margin(1e-6));
    CHECK(c.b == Catch::Approx(1.0f - v).margin(1e-6));
  }
}

TEST_CASE("each control moves the picture the way it says", "[fx][look]") {
  const auto img = test_image();
  const Stats base = stats(graded(fx::Grade{}, img));

  SECTION("warmth raises the red to blue ratio, coolness lowers it") {
    fx::Grade warm;
    warm.warmth = 0.5f;
    fx::Grade cool;
    cool.warmth = -0.5f;
    const Stats w = stats(graded(warm, img));
    const Stats c = stats(graded(cool, img));
    CHECK(w.r / w.b > base.r / base.b * 1.05);
    CHECK(c.r / c.b < base.r / base.b * 0.95);
    // At about the same brightness.
    CHECK(std::abs(w.luma - base.luma) < 3.0);
  }
  SECTION("exposure brightens") {
    fx::Grade g;
    g.exposure = 1.2f;
    CHECK(stats(graded(g, img)).luma > base.luma + 5.0);
  }
  SECTION("contrast spreads the tones") {
    fx::Grade g;
    g.contrast = 1.3f;
    CHECK(stats(graded(g, img)).luma_sd > base.luma_sd * 1.15);
  }
  SECTION("saturation strengthens the colour") {
    fx::Grade more;
    more.saturation = 1.5f;
    fx::Grade less;
    less.saturation = 0.5f;
    CHECK(stats(graded(more, img)).chroma > base.chroma * 1.3);
    CHECK(stats(graded(less, img)).chroma < base.chroma * 0.7);
  }
  SECTION("lift raises the blacks, gain the whites, gamma the mid tones") {
    const std::vector<uint32_t> dark(img.size(), rgba(10, 10, 10));
    fx::Grade lift;
    lift.lift = {0.05f, 0.05f, 0.05f};
    CHECK(red(graded(lift, dark)[0]) > 15.0f);
    const std::vector<uint32_t> mid(img.size(), rgba(128, 128, 128));
    fx::Grade gamma;
    gamma.gamma = {1.2f, 1.2f, 1.2f};
    CHECK(red(graded(gamma, mid)[0]) > 140.0f);
    fx::Grade gain;
    gain.gain = {1.1f, 1.0f, 1.0f};
    const auto g = graded(gain, mid)[0];
    CHECK(red(g) > green(g) + 8.0f);
  }
  SECTION("the vignette darkens the corners, not the middle") {
    const std::vector<uint32_t> flat(img.size(), rgba(160, 160, 160));
    fx::Grade g;
    g.vignette = 0.5f;
    const auto out = graded(g, flat);
    CHECK(luma(at(out, 0, 0)) < 110.0f);
    CHECK(luma(at(out, W / 2, H / 2)) == Catch::Approx(160.0f).margin(1.0f));
  }
  SECTION("bloom lights the surroundings of bright things only") {
    fx::Grade g;
    g.bloom = 1.0f;
    g.bloom_threshold = 0.5f;
    const auto out = graded(g, img);
    const auto ref = graded(fx::Grade{}, img);
    // Next to the lamp: brighter. Far away in the dark: as it was.
    CHECK(luma(at(out, W / 2 + 4, H / 2)) > luma(at(ref, W / 2 + 4, H / 2)));
    CHECK(luma(at(out, 2, 2)) == Catch::Approx(luma(at(ref, 2, 2))).margin(1));
  }
  SECTION("the CRT look darkens the gaps between the original rows") {
    const std::vector<uint32_t> flat(img.size(), rgba(200, 200, 200));
    fx::Grade g;
    g.crt = 1.0f;
    fx::FrameInfo info;
    info.scan_rows = H / 6.0f; // six texels per original row
    const auto out = graded(g, flat, info);
    // Row centres (texel 2-3 of 6) are brighter than the row edges.
    double centre = 0;
    double edge = 0;
    for (int x = 0; x < W; ++x) {
      centre += luma(at(out, x, 2)) + luma(at(out, x, 3));
      edge += luma(at(out, x, 0)) + luma(at(out, x, 5));
    }
    CHECK(centre > edge * 1.1);
    // Off, the flat picture stays flat.
    const auto off = graded(fx::Grade{}, flat, info);
    CHECK(off == flat);
  }
  SECTION("the F8 comparison keeps the 1994 side as it is") {
    fx::Grade g;
    g.saturation = 1.5f;
    g.warmth = 0.5f;
    fx::FrameInfo info;
    info.split_u = 0.5f;
    const auto out = graded(g, img, info);
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W / 2; ++x) {
        REQUIRE(at(out, x, y) == at(img, x, y));
      }
    }
    CHECK(at(out, W - 1, 5) != at(img, W - 1, 5));
  }
}

TEST_CASE("the presets do what their names say", "[fx][look]") {
  const auto img = test_image();
  const Stats classic = stats(graded(fx::Grade{}, img));
  auto preset = [&img](fx::Preset p) {
    return graded(fx::preset_grade(p), img);
  };
  const Stats vivid = stats(preset(fx::Preset::Vivid));
  CHECK(vivid.chroma > classic.chroma * 1.15);
  CHECK(vivid.luma_sd > classic.luma_sd);

  const auto cinematic = preset(fx::Preset::Cinematic);
  const Stats cine = stats(cinematic);
  CHECK(cine.r / cine.b > classic.r / classic.b);
  CHECK(cine.luma_sd > classic.luma_sd);
  const auto ref = graded(fx::Grade{}, img);
  CHECK(luma(at(cinematic, 0, 0)) < luma(at(ref, 0, 0)));

  const Stats natural = stats(preset(fx::Preset::Natural));
  CHECK(natural.r / natural.b > classic.r / classic.b);

  // Studio: open shadows (a dark texel comes up) and a cooler white.
  const std::vector<uint32_t> dark(img.size(), rgba(12, 12, 12));
  const auto studio_dark = graded(fx::preset_grade(fx::Preset::Studio), dark);
  CHECK(luma(studio_dark[0]) > 13.5f);
  const Stats studio = stats(preset(fx::Preset::Studio));
  CHECK(studio.r / studio.b < classic.r / classic.b);
  // The light pass: Cinematic deepens the shadows, Studio opens them.
  CHECK(fx::preset_grade(fx::Preset::Cinematic).shadow_depth > 1.0f);
  CHECK(fx::preset_grade(fx::Preset::Studio).shadow_depth < 1.0f);
}

TEST_CASE("sliders and per-area presets resolve as documented", "[fx][look]") {
  fx::LookSettings s;
  s.preset = fx::Preset::Natural;
  s.per_area[fx::Area::Restaurant] = fx::AreaPreset::Cinematic;
  s.per_area[fx::Area::City] = fx::AreaPreset::Same;
  CHECK(s.preset_for(fx::Area::Restaurant) == fx::Preset::Cinematic);
  CHECK(s.preset_for(fx::Area::City) == fx::Preset::Natural);
  CHECK(s.preset_for(fx::Area::Kitchen) == fx::Preset::Natural);
  CHECK(fx::resolve(s, fx::Area::Restaurant) ==
        fx::preset_grade(fx::Preset::Cinematic));

  s.sliders.intensity = fx::Sliders::MAX;
  s.sliders.shadows = fx::Sliders::MIN;
  s.sliders.warmth = 25;
  s.sliders.crt = true;
  const fx::Grade base = fx::preset_grade(fx::Preset::Natural);
  const fx::Grade g = fx::resolve(s, fx::Area::Other);
  CHECK(g.exposure == Catch::Approx(base.exposure * 1.5f));
  CHECK(g.light_strength == Catch::Approx(1.5f));
  CHECK(g.shadow_depth == Catch::Approx(0.5f));
  CHECK(g.lift[0] > base.lift[0]);
  CHECK(g.warmth == Catch::Approx(base.warmth + 0.3f));
  CHECK(g.crt == 1.0f);

  fx::set_frame_look(s, fx::Area::Restaurant);
  CHECK(fx::frame_grade() == fx::resolve(s, fx::Area::Restaurant));
  fx::set_frame_grade(fx::Grade{});
  CHECK(fx::frame_grade().identity());
}

TEST_CASE("the final draw at the identity is the plain draw",
          "[fx][look][hd]") {
  SoftGpu gpu(W, H);
  auto img = test_image();
  SDL_Texture* frame = upload(gpu.renderer, img);

  SDL_SetRenderDrawColor(gpu.renderer, 0, 0, 0, 255);
  SDL_RenderClear(gpu.renderer);
  SDL_RenderTexture(gpu.renderer, frame, nullptr, nullptr);
  const auto plain = gpu.read();

  fx::PostFx post(gpu.renderer);
  SDL_RenderClear(gpu.renderer);
  CHECK(post.draw(frame, fx::Grade{}, fx::FrameInfo{}) ==
        fx::PostFx::Path::Plain);
  CHECK(gpu.read() == plain);
  CHECK_FALSE(post.shader_ready()); // the software renderer has no shaders
  CHECK_FALSE(fx::PostFx::is_gpu_driver(gpu.renderer));
  SDL_DestroyTexture(frame);
}

TEST_CASE("without the GPU driver, warmth and intensity are a colour "
          "modulation",
          "[fx][look][hd]") {
  SoftGpu gpu(W, H);
  auto img = test_image();
  SDL_Texture* frame = upload(gpu.renderer, img);
  SDL_RenderTexture(gpu.renderer, frame, nullptr, nullptr);
  const Stats base = stats(gpu.read());

  fx::PostFx post(gpu.renderer);
  fx::Grade warm;
  warm.warmth = 0.6f;
  REQUIRE(warm.modulation_only());
  SDL_RenderClear(gpu.renderer);
  CHECK(post.draw(frame, warm, fx::FrameInfo{}) ==
        fx::PostFx::Path::Modulation);
  const Stats w = stats(gpu.read());
  CHECK(w.r / w.b > base.r / base.b * 1.05);

  fx::Grade dim;
  dim.exposure = 0.5f;
  SDL_RenderClear(gpu.renderer);
  post.draw(frame, dim, fx::FrameInfo{});
  CHECK(stats(gpu.read()).luma < base.luma * 0.6);

  // The 1994 side of the F8 comparison is drawn as it is.
  fx::FrameInfo split;
  split.split_u = 0.5f;
  SDL_RenderClear(gpu.renderer);
  post.draw(frame, warm, split);
  const auto out = gpu.read();
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W / 2; ++x) {
      REQUIRE(at(out, x, y) == at(img, x, y));
    }
  }
  // The texture's own modulation is left as it was.
  float r = 0;
  float g = 0;
  float b = 0;
  SDL_GetTextureColorModFloat(frame, &r, &g, &b);
  CHECK((r == 1.0f && g == 1.0f && b == 1.0f));
  float scale = 0;
  SDL_GetRenderColorScale(gpu.renderer, &scale);
  CHECK(scale == 1.0f);
  SDL_DestroyTexture(frame);
}

TEST_CASE("the light pass at strength 1 draws what it always drew",
          "[fx][look][hd][lighting]") {
  constexpr int w = 64;
  constexpr int h = 40;
  SoftGpu gpu(w * 2, h * 2);
  SDL_Texture* albedo =
      SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_Texture* out = SDL_CreateTexture(gpu.renderer, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, w * 2, h * 2);
  SDL_SetRenderTarget(gpu.renderer, albedo);
  SDL_SetRenderDrawColor(gpu.renderer, 200, 180, 160, 255);
  SDL_RenderClear(gpu.renderer);

  lighting::Scene scene;
  scene.ambient = 0.62f;
  lighting::Light lamp;
  lamp.x = 32;
  lamp.y = 20;
  lamp.radius = 12;
  lamp.intensity = 0.7f;
  lamp.glow = 0.4f;
  lamp.glow_radius = 4;
  scene.lights.push_back(lamp);
  LightPass pass(gpu.renderer);

  pass.apply(albedo, out, w, h, 2, scene, 0.0);
  const auto before = gpu.read();
  pass.apply(albedo, out, w, h, 2, scene, 0.0, LightPass::Strength{});
  CHECK(gpu.read() == before);
  CHECK(LightPass::ambient(scene, {}) == 0.62f);

  // Deeper shadows: darker away from the lamp. Weaker lights: darker at it.
  pass.apply(albedo, out, w, h, 2, scene, 0.0, {1.0f, 1.5f});
  const auto deep = gpu.read();
  CHECK(red(deep[4 * w * 2 + 4]) < red(before[4 * w * 2 + 4]) - 10.0f);
  pass.apply(albedo, out, w, h, 2, scene, 0.0, {0.5f, 1.0f});
  const auto weak = gpu.read();
  const std::size_t lamp_texel = 40 * w * 2 + 64;
  CHECK(red(weak[lamp_texel]) < red(before[lamp_texel]));
  CHECK(LightPass::ambient(scene, {1.0f, 0.5f}) ==
        Catch::Approx(1.0f - 0.38f * 0.5f));
  SDL_DestroyTexture(out);
  SDL_DestroyTexture(albedo);
}

TEST_CASE("the shader uniforms carry the grade in the shader's layout",
          "[fx][look]") {
  fx::Grade g = fx::preset_grade(fx::Preset::Cinematic);
  g.crt = 1.0f;
  fx::FrameInfo info;
  info.split_u = 0.25f;
  info.mask_cols = 1920;
  fx::OutputInfo out;
  out.linear = true;
  out.sdr_white = 2.5f;
  out.headroom = 4.0f;
  out.bloom_w = 240;
  out.bloom_h = 150;
  const fx::PostFxUniforms u = fx::make_uniforms(g, info, out);
  CHECK(u.tone[0] == g.exposure);
  CHECK(u.tone[1] == g.contrast);
  CHECK(u.tone[2] == g.saturation);
  CHECK(u.balance[0] == g.balance()[0]);
  CHECK(u.balance[2] == g.balance()[2]);
  CHECK(u.lift[2] == g.lift[2]);
  CHECK(u.gamma[1] == g.gamma[1]);
  CHECK(u.gain[0] == g.gain[0]);
  CHECK(u.bloom[0] == g.bloom);
  CHECK(u.bloom[1] == g.bloom_threshold);
  CHECK(u.bloom[2] == Catch::Approx(1.0f / 240));
  CHECK(u.frame[0] == g.vignette);
  CHECK(u.frame[1] == 1.0f);
  CHECK(u.frame[2] == 400.0f);
  CHECK(u.frame[3] == 1920.0f);
  CHECK(u.output[0] == 0.25f);
  CHECK(u.output[1] == 1.0f);
  CHECK(u.output[2] == 2.5f);
  CHECK(u.output[3] == 4.0f);
}

TEST_CASE("the embedded shader blobs are what their formats need",
          "[fx][look]") {
  using namespace fx::shaders;
  REQUIRE(sizeof(postfx_frag_dxil) > 20);
  CHECK(std::memcmp(postfx_frag_dxil, "DXBC", 4) == 0);
  // Signed by DXC's validator: Direct3D 12 refuses a zero digest.
  bool signed_digest = false;
  for (int i = 4; i < 20; ++i) {
    signed_digest |= postfx_frag_dxil[i] != 0;
  }
  CHECK(signed_digest);
  uint32_t magic = 0;
  std::memcpy(&magic, postfx_frag_spirv, 4);
  CHECK(magic == 0x07230203u);
  const std::string msl(reinterpret_cast<const char*>(postfx_frag_msl),
                        sizeof(postfx_frag_msl));
  CHECK(msl.find(std::string("fragment main0_out ") + postfx_entry_msl) !=
        std::string::npos);
  CHECK(msl.find("[[texture(1)]]") != std::string::npos);
  CHECK(msl.find("[[buffer(0)]]") != std::string::npos);
}

TEST_CASE("the picture settings are saved and read back", "[fx][look]") {
  test_support::TempDir dir;
  const std::string file = (dir.path() / "settings.conf").string();
  {
    // The defaults write no picture lines at all.
    ConfigManager config(file);
    REQUIRE(config.save_options(GameOptions{}));
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("\npicture_preset=") == std::string::npos);
    CHECK(text.str().find("\nrenderer=") == std::string::npos);
  }
  GameOptions opts;
  opts.picture.preset = fx::Preset::Cinematic;
  opts.picture.sliders = {.intensity = 10,
                          .shadows = -20,
                          .bloom = 30,
                          .warmth = -40,
                          .vignette = 50,
                          .crt = true};
  opts.picture.per_area[fx::Area::Kitchen] = fx::AreaPreset::Studio;
  opts.picture.per_area[fx::Area::City] = fx::AreaPreset::Same;
  opts.renderer = fx::RendererChoice::Gpu;
  opts.hdr_output = true;
  {
    ConfigManager config(file);
    REQUIRE(config.save_options(opts));
  }
  ConfigManager reread(file);
  REQUIRE(reread.load_config());
  const GameOptions back = reread.load_options();
  CHECK(back.picture.preset == fx::Preset::Cinematic);
  CHECK(back.picture.sliders == opts.picture.sliders);
  CHECK(back.picture.preset_for(fx::Area::Kitchen) == fx::Preset::Studio);
  CHECK(back.picture.preset_for(fx::Area::City) == fx::Preset::Cinematic);
  CHECK_FALSE(back.picture.per_area.contains(fx::Area::City));
  CHECK(back.renderer == fx::RendererChoice::Gpu);
  CHECK(back.hdr_output);

  // Out of range and unknown values fall back.
  ConfigManager odd(file);
  odd.value("picture_bloom", "999");
  odd.value("picture_preset", "sepia");
  const GameOptions clamped = odd.load_options();
  CHECK(clamped.picture.sliders.bloom == fx::Sliders::MAX);
  CHECK(clamped.picture.preset == fx::Preset::Classic);
}
