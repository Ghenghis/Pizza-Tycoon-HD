/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The music route in AudioManager::fill_block: which source makes the music
// block, and the playback generation the ring must match. The deterministic
// tests pause the dummy device, so the callback cannot run, and call
// fill_block() on the test thread (see its comment in AudioManager.h).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "engine/audio/AudioManager.h"

using audio::AudioManager;
using audio::MIX_BLOCK_FRAMES;
using audio::MIX_BLOCK_SAMPLES;
using audio::Mixer;
using music::PcmRing;
using music::Source;
using music_test::constant_effect_m;
using music_test::DummyAudio;
#if PLEGACY_ENABLE_MUSIC
using music_test::FmReference;
#endif
using music_test::long_note_midi;
using music_test::PausedAudio;
using namespace std::chrono_literals;

namespace {

using Block = std::vector<int16_t>;

Block block() { return Block(MIX_BLOCK_SAMPLES, int16_t{0}); }

Block fill(AudioManager& am) {
  Block out = block();
  am.fill_block(out);
  return out;
}

void write_constant(PcmRing& ring, int16_t value, std::size_t frames,
                    uint32_t generation) {
  const std::vector<int16_t> samples(frames * 2, value);
  ring.write(samples,
             PcmRing::Tag{.generation = generation, .track = 1, .frame = 0});
}

} // namespace

TEST_CASE("Original plays the FM songs exactly as before, and other sources "
          "leave them where they were",
          "[music][route]") {
#if !PLEGACY_ENABLE_MUSIC
  SKIP("built without FM music");
#else
  PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  am.load_music(audio::MusicId::TEA_SHOPPE, long_note_midi());
  am.play_music(audio::MusicId::TEA_SHOPPE);
  FmReference ref(long_note_midi());

  // Original (the default route): sample for sample what it always was.
  CHECK(am.music_source() == Source::ORIGINAL);
  CHECK(am.music_generation() == 0);
  for (int i = 0; i < 12; ++i) {
    REQUIRE(fill(am) == ref.output());
  }
  const double fm_at = am.music_position();
  CHECK(fm_at > 0.2);

  // Off: silent at once (after the declick frames), and the FM song does
  // not move on.
  am.music_route(Source::OFF, 1);
  const Block cut = fill(am);
  CHECK(std::all_of(cut.begin() + AudioManager::DECLICK_FRAMES * 2, cut.end(),
                    [](int16_t s) { return s == 0; }));
  for (int i = 0; i < 5; ++i) {
    const Block off = fill(am);
    CHECK(
        std::all_of(off.begin(), off.end(), [](int16_t s) { return s == 0; }));
  }
  CHECK(am.music_position() == fm_at);

  // My music: the ring's tone, at the fixed trim, and still no FM.
  PcmRing& ring = am.music_ring();
  const std::vector<double> wave =
      music_test::tone(440.0, 8 * MIX_BLOCK_FRAMES, 2, 44100, 0.6);
  std::vector<int16_t> tone(wave.size());
  for (std::size_t i = 0; i < wave.size(); ++i) {
    tone[i] = static_cast<int16_t>(std::lrint(wave[i] * 32767.0));
  }
  am.music_route(Source::MY_MUSIC, 2);
  ring.write(tone, PcmRing::Tag{.generation = 2, .track = 1, .frame = 0});
  ring.producing(2);
  const Block from_off = fill(am); // the cut: nothing was playing
  CHECK(std::all_of(from_off.begin(), from_off.end(),
                    [](int16_t s) { return s == 0; }));
  fill(am); // fades in
  Mixer mix;
  for (std::size_t b = 1; b < 6; ++b) {
    Block expect_music = block();
    for (std::size_t i = 0; i < expect_music.size(); ++i) {
      expect_music[i] = static_cast<int16_t>(
          std::lrint(static_cast<float>(tone[b * MIX_BLOCK_SAMPLES + i]) *
                     AudioManager::MY_MUSIC_GAIN));
    }
    Block expect = block();
    mix.render(expect, expect_music);
    REQUIRE(fill(am) == expect);
  }
  CHECK(ring.frames_read() == 6 * MIX_BLOCK_FRAMES);
  CHECK(ring.underruns() == 0);
  CHECK(am.music_position() == fm_at);

  // Back to Original: the FM song goes on from where it stopped, exactly.
  am.music_route(Source::ORIGINAL, 3);
  fill(am);    // the cut
  ref.music(); // the FM fades in over this block
  fill(am);
  for (int i = 0; i < 8; ++i) {
    REQUIRE(fill(am) == ref.output());
  }
  CHECK(ring.frames_read() == 6 * MIX_BLOCK_FRAMES);
#endif
}

TEST_CASE("quick repeated switches with a late producer never let old music "
          "out, and never touch effects",
          "[music][route]") {
  PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  test_support::TempDir dir;
  const auto m_file = dir.path() / "effect.m";
  music_test::write_file(m_file, constant_effect_m(132));
  am.load_m_container(m_file.string());
  am.play_sound(audio::SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);

  // The effect alone: one level in every sample.
  fill(am);
  const Block alone = fill(am);
  const int16_t effect = alone.front();
  REQUIRE(effect > 0);
  REQUIRE(std::all_of(alone.begin(), alone.end(),
                      [&](int16_t s) { return s == effect; }));

  // Old generations write loud positive music, the current one quiet
  // negative music: any positive excess over the effect is old music.
  constexpr int16_t old_music = 20000;
  constexpr int16_t new_music = -8000;
  PcmRing& ring = am.music_ring();
  const Source cycle[] = {Source::MY_MUSIC, Source::OFF,      Source::MY_MUSIC,
                          Source::ORIGINAL, Source::MY_MUSIC, Source::MY_MUSIC};
  uint32_t generation = 0;
  for (int round = 0; round < 24; ++round) {
    const Source source = cycle[round % 6];
    am.music_route(source, ++generation);
    // The producer is late: it still writes for the generation before.
    write_constant(ring, old_music, 2 * MIX_BLOCK_FRAMES, generation - 1);
    if (source == Source::MY_MUSIC) {
      write_constant(ring, new_music, 4 * MIX_BLOCK_FRAMES, generation);
    }
    // The cut: past the declick frames only the effect is heard.
    const Block cut = fill(am);
    CHECK(std::all_of(cut.begin() + AudioManager::DECLICK_FRAMES * 2, cut.end(),
                      [&](int16_t s) { return s == effect; }));
    // Late audio for older generations, written after the switch too.
    write_constant(ring, old_music, MIX_BLOCK_FRAMES, generation - 1);
    for (int i = 0; i < 2; ++i) {
      const Block out = fill(am);
      CHECK(std::none_of(out.begin(), out.end(),
                         [&](int16_t s) { return s > effect; }));
      if (source == Source::MY_MUSIC && i == 1) {
        CHECK(out.back() < effect); // the new music is heard
      }
      if (source == Source::OFF) {
        CHECK(std::all_of(out.begin(), out.end(),
                          [&](int16_t s) { return s == effect; }));
      }
    }
  }
  CHECK(ring.blocks_dropped() > 0);
}

// Run under ThreadSanitizer: the dummy device's real callback thread mixes
// while a producer thread follows the route and the test switches it.
TEST_CASE("route switches on a running device: only the chosen source moves",
          "[music][route][threads]") {
  DummyAudio sdl;
  if (!sdl.ok()) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager am;
  am.init();
  REQUIRE(am.has_device());
#if PLEGACY_ENABLE_MUSIC
  am.load_music(audio::MusicId::TEA_SHOPPE, long_note_midi());
  am.play_music(audio::MusicId::TEA_SHOPPE);
  REQUIRE(music_test::wait_until([&] { return am.music_position() > 0.1; }));
#endif
  PcmRing& ring = am.music_ring();
  std::atomic<bool> stop{false};
  std::jthread producer([&] {
    while (!stop) {
      const uint32_t generation = am.music_generation();
      if (am.music_source() == Source::MY_MUSIC &&
          ring.writable_frames() >= 1024) {
        write_constant(ring, 3000, 1024, generation);
        ring.producing(generation);
      }
      std::this_thread::sleep_for(1ms);
    }
  });

  am.music_route(Source::MY_MUSIC, 1);
  const uint64_t before = ring.frames_read();
  std::this_thread::sleep_for(300ms);
  CHECK(ring.frames_read() > before + 44100 / 8);
#if PLEGACY_ENABLE_MUSIC
  // The FM song stands still meanwhile.
  const double fm = am.music_position();
  std::this_thread::sleep_for(100ms);
  CHECK(am.music_position() == fm);
#endif

  am.music_route(Source::OFF, 2);
  std::this_thread::sleep_for(60ms);
  const uint64_t off = ring.frames_read();
  std::this_thread::sleep_for(150ms);
  CHECK(ring.frames_read() == off);

  // Quick switching, then Original again: the FM song moves once more.
  for (uint32_t g = 3; g < 60; ++g) {
    am.music_route(g % 3 == 0   ? Source::ORIGINAL
                   : g % 3 == 1 ? Source::MY_MUSIC
                                : Source::OFF,
                   g);
    std::this_thread::sleep_for(500us);
  }
  am.music_route(Source::ORIGINAL, 60);
#if PLEGACY_ENABLE_MUSIC
  const double resumed = am.music_position();
  CHECK(music_test::wait_until(
      [&] { return am.music_position() > resumed + 0.1; }));
#endif
  stop = true;
}
