/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "engine/SDL_include.h"
#include "engine/audio/AudioManager.h"

using audio::AudioManager;
using audio::DeviceAction;
using audio::MIX_BLOCK_FRAMES;
using audio::Mixer;
using audio::MusicId;
using namespace std::chrono_literals;

namespace {

// SDL's dummy audio driver: a real device thread that pulls audio at the
// real rate and throws it away, so the callback runs as it would in a game.
class DummyAudio {
public:
  DummyAudio() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    ok_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
  }
  ~DummyAudio() {
    if (ok_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
  }
  DummyAudio(const DummyAudio&) = delete;
  DummyAudio& operator=(const DummyAudio&) = delete;
  [[nodiscard]] bool ok() const {
    const char* driver = SDL_GetCurrentAudioDriver();
    return ok_ && driver && std::string(driver) == "dummy";
  }

private:
  bool ok_ = false;
};

#if PLEGACY_ENABLE_MUSIC
// A standard MIDI file: one note held for ten seconds (20 beats at 120 a
// minute), so the song's position can be watched.
std::vector<uint8_t> long_note_midi() {
  return {
      'M',  'T',  'h',  'd',  0,    0,    0,    6,
      0,    0,    0,    1,    0,    96,             // 96 ticks a beat
      'M',  'T',  'r',  'k',  0,    0,    0,    23, // 23 bytes
      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,     // tempo: 500000 us a beat
      0x00, 0xC0, 0x00,                             // instrument 0
      0x00, 0x90, 0x3C, 0x64,                       // note on, middle C
      0x8F, 0x00, 0x80, 0x3C, 0x40,                 // 1920 ticks on: note off
      0x00, 0xFF, 0x2F, 0x00,                       // end of track
  };
}

// Waits up to timeout for the song to pass seconds; returns where it is.
double wait_for_music(const AudioManager& am, double seconds,
                      std::chrono::milliseconds timeout = 5000ms) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (am.music_position() < seconds &&
         std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(5ms);
  }
  return am.music_position();
}
#endif

SDL_Event device_event(SDL_EventType type, uint32_t which,
                       bool recording = false) {
  SDL_Event e{};
  e.type = type;
  e.adevice.which = which;
  e.adevice.recording = recording;
  return e;
}

SDL_Event window_event(SDL_EventType type) {
  SDL_Event e{};
  e.type = type;
  return e;
}

std::vector<int16_t> constant(std::size_t frames, int16_t value) {
  return std::vector<int16_t>(frames * 2, value);
}

std::vector<int16_t> render(Mixer& mixer, std::size_t frames,
                            std::span<const int16_t> music = {}) {
  std::vector<int16_t> out(frames * 2, int16_t{-1});
  mixer.render(out, music);
  return out;
}

int16_t mixed(double sum) {
  const double v = std::clamp(sum * Mixer::MASTER_VOLUME, -32768.0, 32767.0);
  return static_cast<int16_t>(std::lround(v));
}

bool all_equal(const std::vector<int16_t>& v, int16_t value) {
  return std::all_of(v.begin(), v.end(),
                     [value](int16_t s) { return s == value; });
}

} // namespace

TEST_CASE("each audio device event gets the right answer", "[audio][device]") {
  using audio::device_event_action;
  constexpr uint32_t ours = 7;
  constexpr uint32_t other = 3;
  constexpr auto removed = SDL_EVENT_AUDIO_DEVICE_REMOVED;
  constexpr auto added = SDL_EVENT_AUDIO_DEVICE_ADDED;

  // Our own device gone while others remain: open the default again.
  CHECK(device_event_action(removed, false, ours, ours, 2) ==
        DeviceAction::REOPEN);
  // Another device gone: SDL moves us to the new default by itself.
  CHECK(device_event_action(removed, false, other, ours, 2) ==
        DeviceAction::NONE);
  // Nothing left to play on: close, so the song holds its place.
  CHECK(device_event_action(removed, false, other, ours, 0) ==
        DeviceAction::CLOSE);
  CHECK(device_event_action(removed, false, ours, ours, 0) ==
        DeviceAction::CLOSE);
  // Already waiting for a device.
  CHECK(device_event_action(removed, false, ours, 0, 0) == DeviceAction::NONE);
  // A device arrives: reopen only when waiting for one.
  CHECK(device_event_action(added, false, other, 0, 1) == DeviceAction::REOPEN);
  CHECK(device_event_action(added, false, other, ours, 2) ==
        DeviceAction::NONE);
  // Microphones and format changes never matter here (SDL converts).
  CHECK(device_event_action(removed, true, ours, ours, 2) ==
        DeviceAction::NONE);
  CHECK(device_event_action(added, true, other, 0, 1) == DeviceAction::NONE);
  CHECK(device_event_action(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED, false, ours,
                            ours, 1) == DeviceAction::NONE);
  CHECK(device_event_action(SDL_EVENT_WINDOW_FOCUS_LOST, false, ours, 0, 0) ==
        DeviceAction::NONE);
}

TEST_CASE("the master volume and the output mute scale the whole mix",
          "[audio][mix]") {
  Mixer m;
  m.sound_volume(1.0f);
  m.music_volume(1.0f);
  const auto fx = m.add_effect(constant(MIX_BLOCK_FRAMES, 10000));
  REQUIRE(fx);
  const auto music = constant(MIX_BLOCK_FRAMES, 5000);
  render(m, MIX_BLOCK_FRAMES, music); // let the gains settle
  REQUIRE(m.play(*fx, true));
  CHECK(all_equal(render(m, MIX_BLOCK_FRAMES, music), mixed(15000)));

  // Half the master volume halves music and effects alike, after a ramp.
  m.master_volume(0.5f);
  const auto ramp = render(m, MIX_BLOCK_FRAMES, music);
  CHECK(ramp.front() < mixed(15000));
  CHECK(ramp.back() == mixed(7500));
  CHECK(all_equal(render(m, MIX_BLOCK_FRAMES, music), mixed(7500)));

  // Muted, nothing comes out; unmuted, the same volume as before.
  m.output_muted(true);
  CHECK(m.output_muted());
  render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(render(m, MIX_BLOCK_FRAMES, music), 0));
  m.output_muted(false);
  render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(render(m, MIX_BLOCK_FRAMES, music), mixed(7500)));

  // Music off still leaves the effects at the master volume.
  m.music_enabled(false);
  render(m, MIX_BLOCK_FRAMES, music);
  CHECK(all_equal(render(m, MIX_BLOCK_FRAMES, music), mixed(5000)));

  m.master_volume(2.0f);
  CHECK(m.master_volume() == 1.0f);
  m.master_volume(std::numeric_limits<float>::quiet_NaN());
  CHECK(m.master_volume() == 0.0f);
}

TEST_CASE("losing window focus mutes only when that option is on",
          "[audio][device]") {
  AudioManager am; // not initialised: no device is needed for this
  const auto lost = window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
  const auto gained = window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);

  am.handle_event(lost);
  CHECK_FALSE(am.focus_muted()); // off by default
  am.mute_when_unfocused(true);
  CHECK(am.focus_muted()); // the window is already in the background
  am.handle_event(gained);
  CHECK_FALSE(am.focus_muted());
  am.handle_event(lost);
  CHECK(am.focus_muted());
  am.mute_when_unfocused(false);
  CHECK_FALSE(am.focus_muted());

  // Device events before init() change nothing.
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_ADDED, 1));
  CHECK_FALSE(am.has_device());
  CHECK(am.device() == 0);
  CHECK(am.music_position() == 0.0);
}

#if PLEGACY_ENABLE_MUSIC
TEST_CASE("a lost device is opened again and the song keeps its place",
          "[audio][device]") {
  DummyAudio sdl;
  if (!sdl.ok()) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager am;
  am.init();
  REQUIRE(am.is_enabled());
  REQUIRE(am.has_device());
  const uint32_t first = am.device();
  CHECK(first != 0);

  am.load_music(MusicId::TEA_SHOPPE, long_note_midi());
  am.play_music(MusicId::TEA_SHOPPE);
  CHECK(am.is_playing());
  // The device thread pulls audio, so the song moves on.
  REQUIRE(wait_for_music(am, 0.3) >= 0.3);

  // SDL reports our own device gone (it could not move it to a new
  // default device).
  const double before = am.music_position();
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, first));
  REQUIRE(am.has_device());
  const uint32_t second = am.device();
  CHECK(second != first);
  // The song goes on from where it was, not from the start.
  CHECK(am.music_position() >= before);
  CHECK(wait_for_music(am, before + 0.3) >= before + 0.3);
  CHECK(am.is_playing());

  // Other devices, microphones and format changes leave the stream alone.
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, second + 1000));
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_ADDED, second + 1001));
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_REMOVED, second, true));
  am.handle_event(device_event(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED, second));
  CHECK(am.device() == second);

  // Reopening on request works the same way.
  const double again = am.music_position();
  REQUIRE(am.reopen_device());
  CHECK(am.device() != second);
  CHECK(wait_for_music(am, again + 0.2) >= again + 0.2);
}

// Run under ThreadSanitizer (build/tsan): the game thread's calls race the
// dummy device's real callback thread, including reopening the device.
TEST_CASE("game-thread audio calls while the device mixes",
          "[audio][device][threads]") {
  DummyAudio sdl;
  if (!sdl.ok()) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager am;
  am.init();
  REQUIRE(am.has_device());
  am.load_music(MusicId::TEA_SHOPPE, long_note_midi());
  am.play_music(MusicId::TEA_SHOPPE);

  const auto lost = window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
  const auto gained = window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
  const auto until = std::chrono::steady_clock::now() + 400ms;
  int calls = 0;
  for (int i = 0; std::chrono::steady_clock::now() < until; ++i) {
    am.music_volume(static_cast<float>(i % 11) / 10.0f);
    am.sound_volume(static_cast<float>((i * 3) % 11) / 10.0f);
    am.master_volume(static_cast<float>((i * 7) % 11) / 10.0f);
    am.mute_when_unfocused(i % 2 == 0);
    am.handle_event(i % 3 == 0 ? lost : gained);
    if (i % 40 == 0) {
      am.stop_music();
      am.play_music(MusicId::TEA_SHOPPE);
    }
    if (i % 150 == 0) {
      CHECK(am.reopen_device());
    }
    static_cast<void>(am.is_playing());
    static_cast<void>(am.music_position());
    ++calls;
    std::this_thread::sleep_for(200us);
  }
  CHECK(calls > 0);
  CHECK(am.has_device());
}
#endif
