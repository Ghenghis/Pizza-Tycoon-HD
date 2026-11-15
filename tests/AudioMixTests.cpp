/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include "engine/audio/AudioManager.h"

using audio::MIX_BLOCK_FRAMES;
using audio::MIX_BLOCK_SAMPLES;
using audio::Mixer;

namespace {

// frames stereo frames with the same value on both channels.
std::vector<int16_t> constant(std::size_t frames, int16_t value) {
  return std::vector<int16_t>(frames * 2, value);
}

std::vector<int16_t> render(Mixer& mixer, std::size_t frames,
                            std::span<const int16_t> music = {}) {
  std::vector<int16_t> out(frames * 2, int16_t{-1});
  mixer.render(out, music);
  return out;
}

// What the mixer outputs for a float sum of int16 samples (master volume,
// round to nearest, saturate).
int16_t mixed(double sum) {
  const double v = std::clamp(sum * Mixer::MASTER_VOLUME, -32768.0, 32767.0);
  return static_cast<int16_t>(std::lround(v));
}

// Volume changes ramp over one block: render a silent one so the gains
// reach their targets before a test listens.
void settle(Mixer& mixer) {
  static_cast<void>(render(mixer, MIX_BLOCK_FRAMES));
}

// Whether every sample from first (inclusive) to last (exclusive) is value.
bool all_equal(const std::vector<int16_t>& v, std::size_t first,
               std::size_t last, int16_t value) {
  return std::all_of(v.begin() + static_cast<std::ptrdiff_t>(first),
                     v.begin() + static_cast<std::ptrdiff_t>(last),
                     [value](int16_t s) { return s == value; });
}

} // namespace

TEST_CASE("mix_add sums blocks and mix_to_s16 saturates", "[audio][mix]") {
  const std::array<int16_t, 6> a{1000, -1000, 30000, -30000, 7, 0};
  const std::array<int16_t, 6> b{2000, -2000, 30000, -30000, -7, 32767};
  std::array<float, 6> acc{};
  audio::mix_add(acc, a, 1.0f, 1.0f);
  audio::mix_add(acc, b, 1.0f, 1.0f);

  std::array<int16_t, 6> out{};
  audio::mix_to_s16(acc, out, 1.0f);
  CHECK(out == std::array<int16_t, 6>{3000, -3000, 32767, -32768, 0, 32767});

  audio::mix_to_s16(acc, out, 0.5f);
  CHECK(out == std::array<int16_t, 6>{1500, -1500, 30000, -30000, 0, 16384});
}

TEST_CASE("mix_add ramps the gain across the block's frames", "[audio][mix]") {
  const auto src = constant(4, 1000);
  std::vector<float> acc(src.size(), 0.0f);
  audio::mix_add(acc, src, 0.0f, 1.0f);
  std::vector<int16_t> out(src.size());
  audio::mix_to_s16(acc, out, 1.0f);
  // Both channels of a frame share a gain; the last frame reaches gain_to.
  CHECK(out == std::vector<int16_t>{250, 250, 500, 500, 750, 750, 1000, 1000});

  // Sizes that differ mix only the overlap.
  std::vector<float> small(2, 0.0f);
  audio::mix_add(small, src, 1.0f, 1.0f);
  CHECK(small[1] * 32768.0f == 1000.0f);
  audio::mix_add(std::span<float>{}, src, 1.0f, 1.0f);
}

TEST_CASE("overlapping effects add up instead of overwriting each other",
          "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  const auto a = m.add_effect(constant(100, 1000));
  const auto b = m.add_effect(constant(50, 2000));
  REQUIRE(a);
  REQUIRE(b);
  CHECK(m.effect_count() == 2);
  settle(m);

  REQUIRE(m.play(*a, false));
  REQUIRE(m.play(*b, false));
  CHECK(m.any_effect_playing()); // asked, not yet started
  const auto out = render(m, 200);
  CHECK(all_equal(out, 0, 100, mixed(1000 + 2000))); // frames 0-49: both
  CHECK(all_equal(out, 100, 200, mixed(1000)));      // frames 50-99: a alone
  CHECK(all_equal(out, 200, 400, 0));                // both finished
  CHECK_FALSE(m.any_effect_playing());
}

TEST_CASE("a mix too loud for int16 saturates instead of wrapping",
          "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  const auto up1 = m.add_effect(constant(10, 30000));
  const auto up2 = m.add_effect(constant(10, 30000));
  const auto down1 = m.add_effect(constant(10, -30000));
  const auto down2 = m.add_effect(constant(10, -30000));
  REQUIRE(up1);
  REQUIRE(up2);
  REQUIRE(down1);
  REQUIRE(down2);
  settle(m);

  // One is within range; two are 60000 x 0.8 = 48000, past full scale.
  REQUIRE(m.play(*up1, false));
  CHECK(all_equal(render(m, 10), 0, 20, mixed(30000)));
  REQUIRE(m.play(*up1, false));
  REQUIRE(m.play(*up2, false));
  CHECK(all_equal(render(m, 10), 0, 20, 32767));
  REQUIRE(m.play(*down1, false));
  REQUIRE(m.play(*down2, false));
  CHECK(all_equal(render(m, 10), 0, 20, -32768));
  // Opposite effects cancel out.
  REQUIRE(m.play(*up1, false));
  REQUIRE(m.play(*down1, false));
  CHECK(all_equal(render(m, 10), 0, 20, 0));

  // Asking twice before a block restarts the effect once; it does not play
  // twice over.
  REQUIRE(m.play(*up1, false));
  REQUIRE(m.play(*up1, false));
  CHECK(all_equal(render(m, 10), 0, 20, mixed(30000)));
}

TEST_CASE("music and effects keep separate gains", "[audio][mix]") {
  Mixer m;
  m.music_volume(0.5f);
  m.sound_volume(1.0f);
  const auto fx = m.add_effect(constant(8 * MIX_BLOCK_FRAMES, 1000));
  REQUIRE(fx);
  const auto music = constant(MIX_BLOCK_FRAMES, 10000);
  render(m, MIX_BLOCK_FRAMES, music);

  REQUIRE(m.play(*fx, false));
  auto out = render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(out, 0, out.size(), mixed(10000 * 0.5 + 1000)));

  m.sound_volume(0.25f);
  m.music_volume(1.0f);
  render(m, MIX_BLOCK_FRAMES, music);
  out = render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(out, 0, out.size(), mixed(10000 + 250)));

  // Turning the music off mutes it but leaves the effects alone.
  m.music_enabled(false);
  CHECK_FALSE(m.music_enabled());
  render(m, MIX_BLOCK_FRAMES, music);
  out = render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(out, 0, out.size(), mixed(250)));
  m.music_enabled(true);
}

TEST_CASE("volume changes take effect, ramped over one block", "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  const auto fx = m.add_effect(constant(8 * MIX_BLOCK_FRAMES, 10000));
  REQUIRE(fx);
  settle(m);
  REQUIRE(m.play(*fx, false));
  auto out = render(m, MIX_BLOCK_FRAMES);
  CHECK(all_equal(out, 0, out.size(), mixed(10000)));

  m.sound_volume(0.5f);
  out = render(m, MIX_BLOCK_FRAMES);
  // A smooth slide down to the new gain, with no jump.
  CHECK(out.front() < mixed(10000));
  CHECK(out.front() > mixed(9900));
  CHECK(out.back() == mixed(5000));
  CHECK(std::is_sorted(out.rbegin(), out.rend()));
  out = render(m, MIX_BLOCK_FRAMES);
  CHECK(all_equal(out, 0, out.size(), mixed(5000)));

  m.sound_volume(0.0f);
  render(m, MIX_BLOCK_FRAMES);
  out = render(m, MIX_BLOCK_FRAMES);
  CHECK(all_equal(out, 0, out.size(), 0));

  // Out of range values are clamped; NaN counts as silence.
  m.sound_volume(2.0f);
  CHECK(m.sound_volume() == 1.0f);
  m.music_volume(-1.0f);
  CHECK(m.music_volume() == 0.0f);
  m.sound_volume(std::numeric_limits<float>::quiet_NaN());
  CHECK(m.sound_volume() == 0.0f);
}

TEST_CASE("play, stop, loop and start offset keep sample timing",
          "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  // Three frames, 100/200/300, played from the second frame on.
  const auto fx = m.add_effect({100, 100, 200, 200, 300, 300}, 2);
  REQUIRE(fx);
  settle(m);

  REQUIRE(m.play(*fx, false));
  auto out = render(m, 4);
  CHECK(out == std::vector<int16_t>{mixed(200), mixed(200), mixed(300),
                                    mixed(300), 0, 0, 0, 0});

  // A loop wraps inside the block, back to the start offset, with no gap.
  REQUIRE(m.play(*fx, true));
  out = render(m, 5);
  const std::vector<int16_t> looped{mixed(200), mixed(300), mixed(200),
                                    mixed(300), mixed(200)};
  for (std::size_t f = 0; f < looped.size(); ++f) {
    CHECK(out[2 * f] == looped[f]);
    CHECK(out[2 * f + 1] == looped[f]);
  }
  // ...and keeps its place across blocks.
  out = render(m, 1);
  CHECK(out[0] == mixed(300));
  CHECK(m.any_effect_playing());

  // A stop takes effect at the next block; play then stop does nothing.
  REQUIRE(m.stop(*fx));
  CHECK_FALSE(m.any_effect_playing());
  CHECK(all_equal(render(m, 8), 0, 16, 0));
  REQUIRE(m.play(*fx, true));
  REQUIRE(m.stop(*fx));
  CHECK(all_equal(render(m, 8), 0, 16, 0));

  // Bad slots are refused.
  CHECK_FALSE(m.play(99, false));
  CHECK_FALSE(m.stop(m.effect_count()));
}

TEST_CASE("effects are stored in whole frames, within a fixed table",
          "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  // An odd sample count loses its last sample; an offset past the end is
  // dropped.
  const auto odd = m.add_effect({500, 500, 600}, 10);
  REQUIRE(odd);
  settle(m);
  REQUIRE(m.play(*odd, false));
  auto out = render(m, 2);
  CHECK(out == std::vector<int16_t>{mixed(500), mixed(500), 0, 0});

  // An empty effect plays as silence, even looped.
  const auto empty = m.add_effect({});
  REQUIRE(empty);
  REQUIRE(m.play(*empty, true));
  CHECK(all_equal(render(m, 4), 0, 8, 0));
  CHECK_FALSE(m.any_effect_playing());

  // The table's last PACK_EVENTS slots are the pack events', never an
  // original's.
  while (m.effect_count() < Mixer::EVENT_BASE) {
    REQUIRE(m.add_effect(constant(1, 1)));
  }
  CHECK_FALSE(m.add_effect(constant(1, 1)));
  CHECK(m.effect_count() == Mixer::EVENT_BASE);
  CHECK(m.valid(Mixer::MAX_EFFECTS - 1));
  CHECK_FALSE(m.valid(Mixer::MAX_EFFECTS));

  // An odd output size leaves the stray sample silent.
  std::vector<int16_t> odd_out(5, int16_t{-1});
  m.render(odd_out);
  CHECK(odd_out.back() == 0);
}

TEST_CASE("requests bigger than a block mix without seams", "[audio][mix]") {
  Mixer m;
  m.music_volume(1.0f);
  const std::size_t frames = 2 * MIX_BLOCK_FRAMES + 300;
  std::vector<int16_t> music(frames * 2);
  for (std::size_t i = 0; i < music.size(); ++i) {
    music[i] = static_cast<int16_t>(static_cast<int>(i % 2000) - 1000);
  }
  const auto fx = m.add_effect(constant(1, 50));
  REQUIRE(fx);
  m.sound_volume(1.0f);
  settle(m);
  REQUIRE(m.play(*fx, true));
  const auto out = render(m, frames, music);
  bool exact = true;
  for (std::size_t i = 0; i < out.size(); ++i) {
    exact = exact && out[i] == mixed(music[i] + 50);
  }
  CHECK(exact);
}

// Run under ThreadSanitizer (build/tsan) to check the lock-free paths.
TEST_CASE("setters hammered from another thread while the mix runs",
          "[audio][mix][threads]") {
  Mixer m;
  const auto a = m.add_effect(constant(5000, 1000));
  const auto b = m.add_effect(constant(300, -1000));
  REQUIRE(a);
  REQUIRE(b);

  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
  std::atomic<long> calls{0};
  std::thread hammer([&] {
    started = true;
    for (int i = 0; !done.load(); ++i) {
      m.music_volume(static_cast<float>(i % 11) / 10.0f);
      m.sound_volume(static_cast<float>((i * 7) % 11) / 10.0f);
      m.music_enabled(i % 5 != 0);
      if (i % 3 == 0) {
        m.play(*a, i % 2 == 0);
      }
      else {
        m.stop(*a);
      }
      m.play(*b, false);
      static_cast<void>(m.any_effect_playing());
      static_cast<void>(m.music_volume() + m.sound_volume());
      calls.fetch_add(1);
    }
  });
  while (!started.load()) {
    std::this_thread::yield();
  }

  // Bigger than a block, so render() splits it.
  std::vector<int16_t> out(MIX_BLOCK_SAMPLES + 512);
  const std::vector<int16_t> music(out.size(), 5000);
  int peak = 0;
  for (int i = 0; i < 2000; ++i) {
    m.render(out, music);
    for (const int16_t s : out) {
      peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
  }
  done = true;
  hammer.join();
  CHECK(calls.load() > 0);
  // Never more than everything at full volume at once.
  CHECK(peak <= mixed(5000 + 1000 + 1000));
}

namespace {

// A continuous sine on both channels, amplitude amp of full scale, in
// blocks of MIX_BLOCK_FRAMES.
struct Tone {
  double amp;
  double hz = 440.0;
  std::size_t frame = 0;
  std::vector<int16_t> next() {
    std::vector<int16_t> block(MIX_BLOCK_SAMPLES);
    for (std::size_t i = 0; i < MIX_BLOCK_FRAMES; ++i, ++frame) {
      const double t = static_cast<double>(frame) / Mixer::RATE;
      const auto v = static_cast<int16_t>(std::lround(
          amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * hz * t)));
      block[2 * i] = v;
      block[2 * i + 1] = v;
    }
    return block;
  }
};

// Blocks in seconds of audio.
std::size_t blocks_in(double seconds) {
  return static_cast<std::size_t>(seconds * Mixer::RATE / MIX_BLOCK_FRAMES);
}

} // namespace

TEST_CASE("the music meter is 0 while the music is off", "[audio][mix]") {
  Mixer m;
  Tone tone{.amp = 0.5};
  CHECK(m.music_level() == 0.0f);

  m.music_enabled(false);
  for (std::size_t b = 0; b < blocks_in(1.0); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
    REQUIRE(m.music_level() == 0.0f);
  }

  // Volume 0 and a muted output count as off too.
  m.music_enabled(true);
  m.music_volume(0.0f);
  for (std::size_t b = 0; b < blocks_in(0.5); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
    REQUIRE(m.music_level() == 0.0f);
  }
  m.music_volume(1.0f);
  m.output_muted(true);
  for (std::size_t b = 0; b < blocks_in(0.5); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
    REQUIRE(m.music_level() == 0.0f);
  }

  // Effects are not music.
  m.output_muted(false);
  const auto fx = m.add_effect(constant(8 * MIX_BLOCK_FRAMES, 20000));
  REQUIRE(fx);
  REQUIRE(m.play(*fx, true));
  for (std::size_t b = 0; b < blocks_in(0.5); ++b) {
    render(m, MIX_BLOCK_FRAMES);
    REQUIRE(m.music_level() == 0.0f);
  }
}

TEST_CASE("a steady tone gives a steady music level in range", "[audio][mix]") {
  Mixer m;
  Tone tone{.amp = 0.5};
  const float rms =
      static_cast<float>(0.5 * 32767.0 / 32768.0 / std::sqrt(2.0));

  // About 100 ms to settle; after 1 s it holds the tone's RMS.
  for (std::size_t b = 0; b < blocks_in(1.0); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
    const float level = m.music_level();
    REQUIRE(level >= 0.0f);
    REQUIRE(level <= 1.0f);
  }
  float low = 1.0f;
  float high = 0.0f;
  for (std::size_t b = 0; b < blocks_in(1.0); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
    low = std::min(low, m.music_level());
    high = std::max(high, m.music_level());
  }
  CHECK(std::abs(low - rms) < 0.01f);
  CHECK(std::abs(high - rms) < 0.01f);
  CHECK(high - low < 0.005f);

  // The music volume is in it: half the volume, half the level.
  m.music_volume(0.5f);
  for (std::size_t b = 0; b < blocks_in(1.0); ++b) {
    const auto music = tone.next();
    render(m, MIX_BLOCK_FRAMES, music);
  }
  CHECK(std::abs(m.music_level() - 0.5f * rms) < 0.01f);

  // When the song ends, the level falls away.
  for (std::size_t b = 0; b < blocks_in(2.0); ++b) {
    render(m, MIX_BLOCK_FRAMES);
  }
  CHECK(m.music_level() < 0.001f);
}
