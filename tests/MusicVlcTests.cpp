/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// libVLC, when one is installed (libvlc5 and vlc-plugin-base on Linux, VLC
// 3.0.x beside the game on Windows). Without it these tests are skipped and
// say why; the built-in decoders cover the same files.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/MusicPlayback.h"
#include "engine/music/VlcDecoder.h"

using Catch::Approx;
using music::TrackReader;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<music::VlcRuntime> vlc_or_skip() {
  std::string why;
  auto vlc = music::load_vlc(&why);
  if (!vlc) {
    SKIP("libVLC is not available: " << why);
  }
  return vlc;
}

// libVLC delivers in about real time: reads until the end, or timeout.
std::vector<std::int16_t> read_until_end(TrackReader& reader,
                                         std::chrono::milliseconds timeout) {
  std::vector<std::int16_t> out;
  std::vector<std::int16_t> chunk(1024 * 2);
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (!reader.at_end() && std::chrono::steady_clock::now() < until) {
    const std::size_t got = reader.read(chunk);
    out.insert(out.end(), chunk.begin(),
               chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
    if (got == 0) {
      std::this_thread::sleep_for(5ms);
    }
  }
  return out;
}

} // namespace

TEST_CASE("libVLC decodes an MP3 into our own buffers", "[music][vlc]") {
  const auto vlc = vlc_or_skip();
  INFO("libVLC " << music::vlc_version(*vlc));
  const auto file = music_test::fixture("tone-440-stereo-44k.mp3");
  std::string error;
  const auto probed = music::probe_file(file, {}, &error);
  REQUIRE(probed);
  auto decoder = music::open_vlc_decoder(vlc, file, *probed, &error);
  INFO(error);
  REQUIRE(decoder);
  CHECK(decoder->backend().starts_with("libVLC 3."));
  CHECK(decoder->info().sample_rate == 44100);
  CHECK(decoder->info().channels == 2);
  TrackReader reader;
  REQUIRE(reader.open(std::move(decoder), &error));
  const auto samples = read_until_end(reader, 10s);
  CHECK(reader.at_end());
  CHECK_FALSE(reader.damaged());
  CHECK(static_cast<double>(samples.size() / 2) == Approx(44100).epsilon(0.1));
  CHECK(music_test::frequency(samples) == Approx(440).epsilon(0.03));
}

TEST_CASE("a libVLC track can be dropped while it plays", "[music][vlc]") {
  const auto vlc = vlc_or_skip();
  const auto file = music_test::fixture("tone-440-stereo-44k.mp3");
  std::string error;
  const auto probed = music::probe_file(file, {}, &error);
  REQUIRE(probed);
  for (int i = 0; i < 5; ++i) {
    auto decoder = music::open_vlc_decoder(vlc, file, *probed, &error);
    REQUIRE(decoder);
    // Its callbacks are running, or about to: destroying it waits for them.
    std::this_thread::sleep_for(std::chrono::milliseconds(10 * i));
  }
  SUCCEED("no callback outlived its decoder");
}

TEST_CASE("the music owner plays through libVLC when asked to",
          "[music][vlc][playback]") {
  const auto vlc = vlc_or_skip();
  audio::AudioManager am;
  music::MusicPlayback mp(am);
  mp.set_backend(music::Backend::LIBVLC);
  mp.set_repeat(music::Repeat::OFF);
  mp.set_queue({music::QueuedTrack{
      .id = "trk:" + std::string(32, 'v'),
      .file = music_test::fixture("tone-440-stereo-44k.mp3"),
      .title = "V"}});
  mp.set_mode(music::Mode::CHRISTMAS_BACKGROUND);
  std::vector<std::int16_t> heard;
  std::vector<std::int16_t> chunk(512 * 2);
  REQUIRE(music_test::wait_until(
      [&] {
        const std::size_t got =
            am.music_ring().read(chunk, am.music_generation());
        heard.insert(heard.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
        return mp.play_state() == music::PlayState::STOPPED &&
               am.music_ring().buffered_frames() == 0;
      },
      10s));
  CHECK(mp.status().backend.starts_with("libVLC"));
  CHECK(static_cast<double>(heard.size() / 2) == Approx(44100).epsilon(0.1));
  CHECK(music_test::frequency(heard) == Approx(440).epsilon(0.03));
}
