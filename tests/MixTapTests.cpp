/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The recording tap of the final mix (audio::MixTap,
// docs/studio/notes/recording.md): positions by sample, bounded, and the
// game's real output through AudioManager::fill_block().

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "engine/audio/AudioManager.h"
#include "engine/audio/MixTap.h"

using audio::MixTap;

namespace {

// Frames whose samples say their own position: left = position, right =
// its negation, both wrapped to int16.
std::vector<std::int16_t> numbered(std::uint64_t first, std::size_t frames) {
  std::vector<std::int16_t> out(frames * 2);
  for (std::size_t f = 0; f < frames; ++f) {
    const auto v = static_cast<std::uint16_t>(first + f);
    out[f * 2] = static_cast<std::int16_t>(v);
    out[f * 2 + 1] = static_cast<std::int16_t>(static_cast<std::uint16_t>(~v));
  }
  return out;
}

bool says(std::span<const std::int16_t> samples, std::uint64_t first) {
  return std::equal(samples.begin(), samples.end(),
                    numbered(first, samples.size() / 2).begin());
}

} // namespace

TEST_CASE("the tap keeps the mix with its sample positions, only while on",
          "[audio][tap]") {
  MixTap tap(8); // 2048 frames
  CHECK(tap.capacity_frames() == 8 * MixTap::BLOCK_FRAMES);
  std::vector<std::int16_t> out(4096 * 2);

  // Off: positions count on, nothing is kept.
  tap.write(numbered(0, 1000));
  CHECK(tap.position() == 1000);
  CHECK(tap.read(out) == 0);

  tap.enabled(true);
  tap.write(numbered(1000, 300));
  tap.write(numbered(1300, 700)); // across blocks of different fill
  CHECK(tap.buffered_frames() == 1000);
  std::uint64_t first = 0;
  // A small read, then the rest: one run, in order.
  std::vector<std::int16_t> small(100 * 2);
  REQUIRE(tap.read(small, &first) == 100);
  CHECK(first == 1000);
  CHECK(says(small, 1000));
  const std::size_t got = tap.read(out, &first);
  CHECK(got == 900);
  CHECK(first == 1100);
  CHECK(says(std::span(out).first(got * 2), 1100));
  CHECK(tap.buffered_frames() == 0);
  CHECK(tap.read(out) == 0);

  // Off again: the position still counts, so the next run says where it is.
  tap.enabled(false);
  tap.write(numbered(2000, 500));
  tap.enabled(true);
  tap.write(numbered(2500, 10));
  REQUIRE(tap.read(out, &first) == 10);
  CHECK(first == 2500);
  CHECK(says(std::span(out).first(20), 2500));
  CHECK(tap.dropped_frames() == 0);
}

TEST_CASE("a full tap drops the newest audio, counted, and the reader sees "
          "the gap where it was",
          "[audio][tap]") {
  MixTap tap(4); // 1024 frames
  tap.enabled(true);
  tap.write(numbered(0, 1024)); // full
  tap.write(numbered(1024, 500));
  CHECK(tap.dropped_frames() == 500);
  CHECK(tap.position() == 1524);
  std::vector<std::int16_t> out(256 * 2);
  std::uint64_t first = 0;
  REQUIRE(tap.read(out, &first) == 256);
  CHECK(first == 0);
  tap.write(numbered(1524, 100)); // room for one block again
  std::vector<std::int16_t> all(4096 * 2);
  std::size_t got = tap.read(all, &first);
  CHECK(first == 256);
  CHECK(got == 768); // the run ends at the gap
  CHECK(says(std::span(all).first(got * 2), 256));
  got = tap.read(all, &first);
  CHECK(first == 1524); // 500 frames lost, not shifted
  CHECK(got == 100);
  CHECK(says(std::span(all).first(got * 2), 1524));

  // clear() leaves the position and drops what was kept.
  tap.write(numbered(1624, 300));
  tap.clear();
  CHECK(tap.buffered_frames() == 0);
  CHECK(tap.read(all) == 0);
  CHECK(tap.position() == 1924);
}

TEST_CASE("the tap's writer and reader run at once without losing order",
          "[audio][tap]") {
  MixTap tap(16);
  tap.enabled(true);
  constexpr std::uint64_t total = 400000;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::uint64_t at = 0;
    std::size_t n = 1;
    while (at < total) {
      const std::size_t frames =
          static_cast<std::size_t>(std::min<std::uint64_t>(n, total - at));
      tap.write(numbered(at, frames));
      at += frames;
      n = n % 700 + 37; // block sizes that do not line up with the ring
    }
    done.store(true);
  });
  std::vector<std::int16_t> out(333 * 2);
  std::uint64_t next = 0;
  std::uint64_t heard = 0;
  bool in_order = true;
  bool right = true;
  for (;;) {
    std::uint64_t first = 0;
    const std::size_t got = tap.read(out, &first);
    if (got == 0) {
      if (done.load() && tap.buffered_frames() == 0) {
        break;
      }
      std::this_thread::yield();
      continue;
    }
    in_order = in_order && first >= next;
    right = right && says(std::span(out).first(got * 2), first);
    next = first + got;
    heard += got;
  }
  writer.join();
  CHECK(in_order);
  CHECK(right);
  CHECK(heard + tap.dropped_frames() == total);
  CHECK(tap.position() == total);
}

TEST_CASE("the tap holds exactly what the game's mix gives the device",
          "[audio][tap]") {
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  audio::AudioManager& am = a.am;
  test_support::TempDir dir;
  music_test::write_file(dir.path() / "fx.m", music_test::effects_m({150}));
  am.load_m_container((dir.path() / "fx.m").string());
  am.play_sound(audio::SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);

  music_test::fill(am); // before the tap is on: counted, not kept
  const std::uint64_t start = am.mix_tap().position();
  CHECK(start == audio::MIX_BLOCK_FRAMES);
  am.mix_tap().enabled(true);
  std::vector<std::int16_t> heard;
  for (int i = 0; i < 3; ++i) {
    const auto out = music_test::fill(am);
    heard.insert(heard.end(), out.begin(), out.end());
  }
  REQUIRE(heard.front() != 0);
  std::vector<std::int16_t> kept(heard.size());
  std::uint64_t first = 0;
  REQUIRE(am.mix_tap().read(kept, &first) == heard.size() / 2);
  CHECK(first == start);
  CHECK(kept == heard);
  CHECK(am.mix_tap().dropped_frames() == 0);
}
