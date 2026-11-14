/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The music owner (music::MusicPlayback) and its worker. With no device
// open, this thread reads the ring the way the audio callback would, at the
// route's generation, so the tests see exactly what would be heard.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/CommandQueue.h"
#include "engine/music/MusicPlayback.h"

using audio::AudioManager;
using Catch::Approx;
using music::Command;
using music::CommandQueue;
using music::Mode;
using music::MusicPlayback;
using music::Notice;
using music::PlayState;
using music::QueuedTrack;
using music::Source;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string id(char c) { return "trk:" + std::string(32, c); }

// What the audio callback would play: up to frames, waiting for the worker
// to keep up, at most timeout.
std::vector<std::int16_t> listen(AudioManager& am, std::size_t frames,
                                 std::chrono::milliseconds timeout = 5000ms) {
  std::vector<std::int16_t> heard;
  std::vector<std::int16_t> chunk(512 * 2);
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (heard.size() / 2 < frames &&
         std::chrono::steady_clock::now() < until) {
    const std::size_t want =
        std::min<std::size_t>(512, frames - heard.size() / 2);
    const std::size_t got = am.music_ring().read(
        std::span(chunk).first(want * 2), am.music_generation());
    heard.insert(heard.end(), chunk.begin(),
                 chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
    if (got == 0) {
      std::this_thread::sleep_for(1ms);
    }
  }
  return heard;
}

struct NoticeLog {
  std::vector<Notice> all;
  void take(MusicPlayback& mp) {
    for (Notice& n : mp.poll_notices()) {
      all.push_back(std::move(n));
    }
  }
  [[nodiscard]] std::size_t count(Notice::Kind kind) const {
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(),
                      [&](const Notice& n) { return n.kind == kind; }));
  }
  [[nodiscard]] std::vector<std::string> started() const {
    std::vector<std::string> out;
    for (const Notice& n : all) {
      if (n.kind == Notice::Kind::TRACK_STARTED) {
        out.push_back(n.track);
      }
    }
    return out;
  }
};

// Listens until the queue has played to its end.
std::size_t listen_to_end(AudioManager& am, MusicPlayback& mp, NoticeLog& log,
                          std::chrono::milliseconds timeout = 10000ms) {
  std::size_t frames = 0;
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until) {
    frames += listen(am, 4096, 50ms).size() / 2;
    log.take(mp);
    if (log.count(Notice::Kind::STOPPED) > 0 &&
        am.music_ring().buffered_frames() == 0) {
      break;
    }
  }
  return frames;
}

struct Tones {
  test_support::TempDir dir;
  QueuedTrack wav(char c, double hz, double seconds,
                  music_test::WavSpec spec = {}) {
    const fs::path file = music_test::tone_wav(
        dir.path() / (std::string(1, c) + ".wav"), hz, seconds, spec);
    return QueuedTrack{.id = id(c), .file = file, .title = std::string(1, c)};
  }
};

} // namespace

TEST_CASE("new commands stand in for queued ones of their kind when the "
          "queue is full",
          "[music][playback]") {
  CommandQueue q;
  for (std::uint32_t g = 1; g <= CommandQueue::CAPACITY; ++g) {
    Command c;
    c.kind = Command::Kind::PLAY;
    c.generation = g;
    q.push(c);
  }
  CHECK(q.size() == CommandQueue::CAPACITY);
  CHECK(q.coalesced() == 0);
  // Full: a newer PLAY replaces the newest queued PLAY.
  Command newer;
  newer.kind = Command::Kind::PLAY;
  newer.generation = 100;
  q.push(newer);
  CHECK(q.size() == CommandQueue::CAPACITY);
  CHECK(q.items().back().generation == 100);
  CHECK(q.coalesced() == 1);
  // Full and none of its kind: the queue keeps the newest of each kind.
  Command halt;
  halt.kind = Command::Kind::HALT;
  halt.generation = 101;
  q.push(halt);
  REQUIRE(q.size() == 2);
  CHECK(q.items()[0].generation == 100);
  CHECK(q.items()[1].kind == Command::Kind::HALT);
  const auto taken = q.take();
  CHECK(taken.size() == 2);
  CHECK(q.empty());
}

TEST_CASE("the worker plays a queue of WAV and MP3 through the ring",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  const QueuedTrack mp3{.id = id('b'),
                        .file = music_test::fixture("tone-440-stereo-44k.mp3"),
                        .title = "B"};
  mp.set_repeat(music::Repeat::OFF);
  mp.set_queue({t.wav('a', 660, 0.3, {.channels = 1, .rate = 22050}), mp3});
  // Queued in Original mode: nothing starts.
  CHECK(mp.play_state() == PlayState::STOPPED);
  CHECK(am.music_source() == Source::ORIGINAL);

  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  CHECK(am.music_source() == Source::MY_MUSIC);
  CHECK(am.music_generation() == mp.generation());
  CHECK(mp.play_state() == PlayState::PLAYING);

  NoticeLog log;
  const std::size_t frames = listen_to_end(am, mp, log);
  CHECK(static_cast<double>(frames) ==
        Approx(0.3 * 44100 + 44100).margin(2500));
  CHECK(log.started() == std::vector<std::string>{id('a'), id('b')});
  CHECK(log.count(Notice::Kind::STOPPED) == 1);
  CHECK(mp.play_state() == PlayState::STOPPED);
}

TEST_CASE("missing and damaged tracks are skipped with a notice naming them",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  const fs::path broken = t.dir.path() / "broken.wav";
  music_test::write_file(broken, std::vector<std::uint8_t>(64, 'x'));
  mp.set_repeat(music::Repeat::OFF);
  mp.set_queue({
      QueuedTrack{.id = id('m'), .file = {}, .title = "Missing"},
      QueuedTrack{
          .id = id('n'), .file = t.dir.path() / "moved.wav", .title = "Moved"},
      QueuedTrack{.id = id('d'), .file = broken, .title = "Broken"},
      t.wav('g', 440, 0.2),
  });
  mp.set_mode(Mode::JUKEBOX);
  NoticeLog log;
  listen_to_end(am, mp, log);
  REQUIRE(log.count(Notice::Kind::SKIPPED) == 3);
  CHECK(log.all[0].text.find("Missing") != std::string::npos);
  CHECK(log.all[0].text.find("not in the music folders") != std::string::npos);
  CHECK(log.all[1].text.find("cannot be opened") != std::string::npos);
  CHECK(log.all[2].track == id('d'));
  CHECK(log.started() == std::vector<std::string>{id('g')});
}

TEST_CASE("a queue where nothing plays stops once, without retrying",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  mp.set_repeat(music::Repeat::ALL);       // even with repeat on
  mp.set_queue({QueuedTrack{.id = id('x'), .file = {}, .title = "X"},
                QueuedTrack{.id = id('y'), .file = {}, .title = "Y"}});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  NoticeLog log;
  REQUIRE(music_test::wait_until([&] {
    log.take(mp);
    return log.count(Notice::Kind::STOPPED) == 1;
  }));
  CHECK(log.count(Notice::Kind::SKIPPED) == 2);
  CHECK(log.all.back().text.find("None of the tracks") != std::string::npos);
  std::this_thread::sleep_for(200ms);
  log.take(mp);
  CHECK(log.all.size() == 3);
  CHECK(mp.play_state() == PlayState::STOPPED);
}

TEST_CASE("next, previous, seek, pause and stop", "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  mp.set_queue(
      {t.wav('a', 440, 4.0), t.wav('b', 660, 4.0), t.wav('c', 880, 4.0)});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);

  listen(am, 22050);
  auto s = mp.status();
  CHECK(s.track == id('a'));
  CHECK(s.position_s == Approx(0.5).margin(0.02));
  CHECK(s.duration_s == Approx(4.0).margin(0.01));
  CHECK(s.queue_size == 3);

  std::uint32_t g = mp.generation();
  mp.next();
  CHECK(mp.generation() > g);
  CHECK(music_test::frequency(listen(am, 11025)) == Approx(660).epsilon(0.03));
  CHECK(mp.status().queue_index == 1);

  mp.previous(); // early in the track: the one before
  CHECK(music_test::frequency(listen(am, 11025)) == Approx(440).epsilon(0.03));
  CHECK(mp.status().track == id('a'));

  listen(am, 44100 * 3); // past three seconds
  mp.previous();         // restarts this one
  listen(am, 4410);
  s = mp.status();
  CHECK(s.track == id('a'));
  CHECK(s.position_s == Approx(0.1).margin(0.02));

  mp.seek(2.0);
  listen(am, 4410);
  CHECK(mp.status().position_s == Approx(2.1).margin(0.02));

  // Pause: silent at once, not an underrun, and the place is kept.
  const std::uint32_t underruns = am.music_ring().underruns();
  mp.pause();
  CHECK(mp.play_state() == PlayState::PAUSED);
  std::this_thread::sleep_for(60ms);
  CHECK(listen(am, 1024, 50ms).empty());
  CHECK(am.music_ring().underruns() == underruns);
  CHECK(mp.status().position_s == Approx(2.1).margin(0.02));
  mp.play();
  listen(am, 4410);
  CHECK(mp.status().position_s == Approx(2.2).margin(0.03));

  // Stop: back to the start of the track.
  mp.stop();
  CHECK(mp.play_state() == PlayState::STOPPED);
  mp.toggle_pause();
  listen(am, 4410);
  CHECK(mp.status().track == id('a'));
  CHECK(mp.status().position_s == Approx(0.1).margin(0.02));
}

TEST_CASE("modes: one source at a time, and the place is kept across them",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  mp.set_queue({t.wav('a', 440, 3.0), t.wav('b', 660, 3.0)});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  listen(am, 22050);

  // Original: the ring's audio is over at once.
  mp.set_mode(Mode::ORIGINAL);
  CHECK(am.music_source() == Source::ORIGINAL);
  CHECK(am.music_generation() == mp.generation());
  CHECK(listen(am, 1024, 100ms).empty());
  mp.set_mode(Mode::OFF);
  CHECK(am.music_source() == Source::OFF);
  CHECK(listen(am, 1024, 100ms).empty());

  // Back: on from where it was.
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  listen(am, 4410);
  auto s = mp.status();
  CHECK(s.track == id('a'));
  CHECK(s.position_s == Approx(0.6).margin(0.03));

  // Jukebox and background are one setting: switching keeps the song.
  mp.set_mode(Mode::JUKEBOX);
  CHECK(am.music_source() == Source::MY_MUSIC);
  listen(am, 4410);
  CHECK(mp.status().position_s == Approx(0.7).margin(0.03));
  CHECK(mp.ambience_suppressed());

  // Stopping the jukebox leaves the music stopped...
  mp.stop();
  CHECK(mp.mode() == Mode::JUKEBOX);
  CHECK(mp.play_state() == PlayState::STOPPED);
  CHECK_FALSE(mp.ambience_suppressed());
  // ...unless the player asked for the background music to come back.
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  mp.set_mode(Mode::JUKEBOX);
  auto policy = mp.policy();
  policy.background_after_jukebox = true;
  mp.set_policy(policy);
  mp.stop();
  CHECK(mp.mode() == Mode::CHRISTMAS_BACKGROUND);
  CHECK(mp.play_state() == PlayState::PLAYING);
}

TEST_CASE("restaurants drive the FM player only, never the player's music",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  // Original: as the views always did it.
  mp.on_restaurant_enter(3);
  CHECK(mp.ambience_suppressed());
  mp.on_restaurant_jukebox(0);
  CHECK_FALSE(mp.ambience_suppressed());
  mp.on_restaurant_jukebox(2);
  CHECK(mp.ambience_suppressed());
  mp.on_restaurant_leave();
  CHECK_FALSE(mp.ambience_suppressed());
  CHECK(mp.generation() == 0); // the route was never touched

  Tones t;
  mp.set_queue({t.wav('a', 440, 2.0)});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  listen(am, 4410);
  const std::uint32_t g = mp.generation();
  mp.on_restaurant_enter(5);
  mp.on_restaurant_jukebox(1);
  CHECK_FALSE(mp.ambience_suppressed()); // its jukebox is not what plays
  mp.on_restaurant_leave();
  CHECK(mp.generation() == g);
  CHECK(mp.mode() == Mode::CHRISTMAS_BACKGROUND);
  CHECK(mp.play_state() == PlayState::PLAYING);
  CHECK(listen(am, 4410).size() == 4410 * 2);
}

TEST_CASE("the North Pole starts and ends the Christmas music by policy",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  mp.set_queue({t.wav('a', 440, 2.0)});

  // Until the player has chosen Christmas music once, nothing starts.
  mp.on_region(true);
  CHECK(mp.mode() == Mode::ORIGINAL);
  mp.on_region(false);
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND); // chosen outside the region
  CHECK(mp.policy().autostart_north_pole);
  mp.set_mode(Mode::ORIGINAL);

  mp.on_region(true);
  CHECK(mp.mode() == Mode::CHRISTMAS_BACKGROUND);
  const std::uint32_t g = mp.generation();
  mp.on_region(true); // a boundary inside the region: no restart
  CHECK(mp.generation() == g);
  mp.on_region(false);
  CHECK(mp.mode() == Mode::ORIGINAL);

  auto policy = mp.policy();
  policy.keep_everywhere = true;
  mp.set_policy(policy);
  mp.on_region(true);
  mp.on_region(false);
  CHECK(mp.mode() == Mode::CHRISTMAS_BACKGROUND);

  // Off is the player's choice: the North Pole leaves it alone.
  mp.set_mode(Mode::OFF);
  mp.on_region(true);
  CHECK(mp.mode() == Mode::OFF);

  // A manual choice inside the North Pole beats the automation from then on.
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  mp.set_mode(Mode::ORIGINAL);
  CHECK_FALSE(mp.policy().autostart_north_pole);
  mp.on_region(false);
  mp.on_region(true);
  CHECK(mp.mode() == Mode::ORIGINAL);
}

TEST_CASE("the queue can change while a song plays on", "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  // Longer than the ring holds, so the worker is still on it.
  const QueuedTrack a = t.wav('a', 440, 2.0);
  const QueuedTrack b = t.wav('b', 660, 0.3);
  const QueuedTrack c = t.wav('c', 880, 0.3);
  mp.set_repeat(music::Repeat::OFF);
  mp.set_queue({a, b, c});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  listen(am, 4410);
  const std::uint32_t g = mp.generation();
  mp.edit_queue({b, a, c});
  CHECK(mp.generation() == g); // not interrupted
  CHECK(mp.status().track == id('a'));
  NoticeLog log;
  listen_to_end(am, mp, log);
  // After a, the queue goes on from a's new place: c, not b.
  CHECK(log.started() == std::vector<std::string>{id('a'), id('c')});
}

TEST_CASE("repeat one and repeat off", "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  Tones t;
  mp.set_repeat(music::Repeat::ONE);
  mp.set_queue({t.wav('a', 440, 0.2), t.wav('b', 660, 0.2)});
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  NoticeLog log;
  listen(am, static_cast<std::size_t>(0.7 * 44100));
  log.take(mp);
  CHECK(log.started().size() >= 3);
  for (const std::string& s : log.started()) {
    CHECK(s == id('a'));
  }
  mp.set_repeat(music::Repeat::OFF);
  listen_to_end(am, mp, log);
  CHECK(log.started().back() == id('b'));
  CHECK(mp.play_state() == PlayState::STOPPED);
}

TEST_CASE("notices are bounded, and the dropped ones are counted",
          "[music][playback]") {
  AudioManager am;
  MusicPlayback mp(am);
  mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
  std::vector<QueuedTrack> missing;
  for (int i = 0; i < 40; ++i) {
    missing.push_back(QueuedTrack{.id = "trk:" + std::string(30, 'a') +
                                        std::to_string(10 + i),
                                  .file = {},
                                  .title = "T" + std::to_string(i)});
  }
  mp.set_queue(missing);
  mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
  REQUIRE(music_test::wait_until(
      [&] { return mp.play_state() == PlayState::STOPPED; }));
  std::this_thread::sleep_for(50ms);
  const auto notices = mp.poll_notices();
  CHECK(notices.size() == MusicPlayback::MAX_NOTICES);
  CHECK(mp.dropped_notices() == 41 - MusicPlayback::MAX_NOTICES);
  CHECK(notices.back().kind == Notice::Kind::STOPPED);
}

TEST_CASE("the player's music goes on through a device change",
          "[music][playback][device]") {
  music_test::DummyAudio sdl;
  if (!sdl.ok()) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager am;
  am.init();
  REQUIRE(am.has_device());
  {
    MusicPlayback mp(am);
    mp.set_backend(music::Backend::BUILTIN);
    mp.set_backend(music::Backend::BUILTIN); // libVLC has its own tests
    Tones t;
    mp.set_queue({t.wav('a', 440, 3.0)});
    mp.set_mode(Mode::CHRISTMAS_BACKGROUND);
    REQUIRE(music_test::wait_until(
        [&] { return am.music_ring().frames_read() > 44100 / 4; }));
    const double before = mp.status().position_s;
    REQUIRE(am.reopen_device());
    const std::uint64_t read = am.music_ring().frames_read();
    CHECK(music_test::wait_until(
        [&] { return am.music_ring().frames_read() > read + 44100 / 4; }));
    const auto s = mp.status();
    CHECK(s.track == id('a'));
    CHECK(s.position_s > before);
  }
  // With the owner gone, the game is back to its own music.
  CHECK(am.music_source() == Source::ORIGINAL);
}
