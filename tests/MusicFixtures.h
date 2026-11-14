/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Helpers for the [music] tests. Every sound here is made by the tests: sine
// tones written as WAV files at runtime, and the two MP3 tones in
// tests/fixtures/music (see GENERATED.txt there). No third-party audio.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine/SDL_include.h"
#include "engine/audio/AudioManager.h"

#if PLEGACY_ENABLE_MUSIC
#include <adlmidi.h>
#endif

namespace music_test {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// SDL's dummy audio driver: a real device thread that pulls audio at the
// real rate and throws it away.
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

// tests/fixtures/music/<name>, from the source tree or the build folder.
inline fs::path fixture(const std::string& name) {
  const fs::path beside =
      fs::path(__FILE__).parent_path() / "fixtures" / "music" / name;
  if (fs::exists(beside)) {
    return beside;
  }
  for (const char* base : {"tests/fixtures/music", "../tests/fixtures/music",
                           "../../tests/fixtures/music"}) {
    if (fs::exists(fs::path(base) / name)) {
      return fs::path(base) / name;
    }
  }
  return beside;
}

inline fs::path u8(const char8_t* text) {
  return fs::path(std::u8string(text));
}

inline void write_file(const fs::path& path,
                       std::span<const std::uint8_t> bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

inline std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Interleaved samples of a sine at hz, in -1..1 times amplitude.
inline std::vector<double> tone(double hz, std::size_t frames,
                                unsigned channels, unsigned rate,
                                double amplitude = 0.5) {
  std::vector<double> out(frames * channels);
  for (std::size_t f = 0; f < frames; ++f) {
    const double v = amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                          static_cast<double>(f) / rate);
    for (unsigned c = 0; c < channels; ++c) {
      out[f * channels + c] = v;
    }
  }
  return out;
}

struct WavSpec {
  std::uint16_t format = 1; // 1 PCM, 3 float
  std::uint16_t channels = 2;
  std::uint32_t rate = 44100;
  std::uint16_t bits = 16;
};

class Bytes {
public:
  void id(const char* four) { bytes.insert(bytes.end(), four, four + 4); }
  void u16(std::uint32_t v) {
    bytes.push_back(static_cast<std::uint8_t>(v));
    bytes.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(v & 0xFFFF);
    u16(v >> 16);
  }
  void raw(std::span<const std::uint8_t> b) {
    bytes.insert(bytes.end(), b.begin(), b.end());
  }
  std::vector<std::uint8_t> bytes;
};

inline std::vector<std::uint8_t> encode(const WavSpec& spec,
                                        const std::vector<double>& samples) {
  Bytes b;
  for (const double s : samples) {
    const double v = std::clamp(s, -1.0, 1.0);
    if (spec.format == 3 && spec.bits == 32) {
      const float f = static_cast<float>(v);
      std::uint32_t u = 0;
      std::memcpy(&u, &f, 4);
      b.u32(u);
    }
    else if (spec.format == 3) {
      std::uint64_t u = 0;
      std::memcpy(&u, &v, 8);
      b.u32(static_cast<std::uint32_t>(u));
      b.u32(static_cast<std::uint32_t>(u >> 32));
    }
    else if (spec.bits == 8) {
      b.bytes.push_back(
          static_cast<std::uint8_t>(std::lround(v * 127.0) + 128));
    }
    else {
      const double scale = std::ldexp(1.0, spec.bits - 1) - 1.0;
      const auto i = static_cast<std::int64_t>(std::llround(v * scale));
      for (int k = 0; k < spec.bits / 8; ++k) {
        b.bytes.push_back(static_cast<std::uint8_t>(i >> (8 * k)));
      }
    }
  }
  return b.bytes;
}

// A WAV file: RIFF, fmt, then the extra chunks given (id, body), then data.
inline std::vector<std::uint8_t>
wav_file(const WavSpec& spec, const std::vector<double>& samples,
         const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>&
             before_data = {}) {
  const std::vector<std::uint8_t> data = encode(spec, samples);
  Bytes body;
  body.id("WAVE");
  body.id("fmt ");
  body.u32(16);
  body.u16(spec.format);
  body.u16(spec.channels);
  body.u32(spec.rate);
  const std::uint32_t align = spec.channels * spec.bits / 8u;
  body.u32(spec.rate * align);
  body.u16(align);
  body.u16(spec.bits);
  for (const auto& [id, chunk] : before_data) {
    body.id(id.c_str());
    body.u32(static_cast<std::uint32_t>(chunk.size()));
    body.raw(chunk);
    if (chunk.size() % 2 == 1) {
      body.bytes.push_back(0);
    }
  }
  body.id("data");
  body.u32(static_cast<std::uint32_t>(data.size()));
  body.raw(data);
  Bytes file;
  file.id("RIFF");
  file.u32(static_cast<std::uint32_t>(body.bytes.size()));
  file.raw(body.bytes);
  return file.bytes;
}

// A LIST/INFO chunk body holding one INAM title (raw bytes, as given).
inline std::vector<std::uint8_t> info_title(const std::string& title) {
  Bytes b;
  b.id("INFO");
  b.id("INAM");
  b.u32(static_cast<std::uint32_t>(title.size() + 1));
  b.raw(std::span(reinterpret_cast<const std::uint8_t*>(title.data()),
                  title.size()));
  b.bytes.push_back(0);
  if ((title.size() + 1) % 2 == 1) {
    b.bytes.push_back(0);
  }
  return b.bytes;
}

// Writes a tone WAV and returns its path.
inline fs::path tone_wav(const fs::path& path, double hz, double seconds,
                         WavSpec spec = {}) {
  const auto frames = static_cast<std::size_t>(seconds * spec.rate);
  write_file(path, wav_file(spec, tone(hz, frames, spec.channels, spec.rate)));
  return path;
}

// The pitch of one channel of interleaved int16 stereo at 44100 Hz: rising
// crossings of a quarter of the peak (with hysteresis, so codec noise near
// zero is not counted), timed from the first to the last, so silence before
// or after the tone does not count either. Plain zero crossings read a 660 Hz
// tone in the 48 kbit/s MP3 fixture as 712 Hz, against a spectral peak of
// 660 Hz.
inline double frequency(std::span<const std::int16_t> stereo, int channel = 0) {
  const std::size_t frames = stereo.size() / 2;
  const auto side = static_cast<std::size_t>(channel);
  int top = 0;
  for (std::size_t f = 0; f < frames; ++f) {
    top = std::max(top, std::abs(static_cast<int>(stereo[f * 2 + side])));
  }
  const int threshold = top / 4;
  if (threshold == 0) {
    return 0.0;
  }
  int state = 0; // -1 below -threshold, +1 above
  std::size_t first = 0;
  std::size_t last = 0;
  std::size_t rises = 0;
  for (std::size_t f = 0; f < frames; ++f) {
    const int v = stereo[f * 2 + side];
    if (v > threshold) {
      if (state < 0) {
        if (rises == 0) {
          first = f;
        }
        last = f;
        ++rises;
      }
      state = 1;
    }
    else if (v < -threshold) {
      state = -1;
    }
  }
  return rises > 1 && last > first ? static_cast<double>(rises - 1) * 44100.0 /
                                         static_cast<double>(last - first)
                                   : 0.0;
}

inline int peak(std::span<const std::int16_t> samples, int channel = -1) {
  int p = 0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (channel < 0 || static_cast<int>(i % 2) == channel) {
      p = std::max(p, std::abs(static_cast<int>(samples[i])));
    }
  }
  return p;
}

// Waits up to timeout for done(); returns its last answer.
inline bool wait_until(const std::function<bool()>& done,
                       std::chrono::milliseconds timeout = 5000ms) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) {
      return done();
    }
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

// A standard MIDI file: one note held for ten seconds.
inline std::vector<std::uint8_t> long_note_midi() {
  return {
      'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,    0,    1,
      0,    96,   'M',  'T',  'r',  'k',  0,    0,    0,    23,   0x00, 0xFF,
      0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0xC0, 0x00, 0x00, 0x90, 0x3C, 0x64,
      0x8F, 0x00, 0x80, 0x3C, 0x40, 0x00, 0xFF, 0x2F, 0x00,
  };
}

// An M container of VOC sounds, one per level: each a constant 8-bit
// level, so a sound's share of the output is one known value. They fill the
// effect slots from 0, as the game's own m0 to m11 do.
inline std::vector<std::uint8_t>
effects_m(const std::vector<std::uint8_t>& levels) {
  std::vector<std::vector<std::uint8_t>> vocs;
  for (const std::uint8_t level : levels) {
    std::vector<std::uint8_t> voc(std::begin("Creative Voice File\x1A"),
                                  std::end("Creative Voice File\x1A") - 1);
    const std::uint16_t version = 0x010A;
    const auto check = static_cast<std::uint16_t>(~version + 0x1234);
    for (const std::uint16_t v : {std::uint16_t{26}, version, check}) {
      voc.push_back(static_cast<std::uint8_t>(v));
      voc.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    constexpr std::uint32_t samples = 22050;
    voc.push_back(1); // sound data
    const std::uint32_t size = samples + 2;
    voc.push_back(static_cast<std::uint8_t>(size));
    voc.push_back(static_cast<std::uint8_t>(size >> 8));
    voc.push_back(static_cast<std::uint8_t>(size >> 16));
    voc.push_back(256 - 45); // 22222 Hz
    voc.push_back(0);        // 8-bit, uncompressed
    voc.insert(voc.end(), samples, level);
    voc.push_back(0); // end
    vocs.push_back(std::move(voc));
  }
  std::vector<std::uint8_t> m;
  const auto u32 = [&m](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      m.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  };
  u32(static_cast<std::uint32_t>(vocs.size()));
  std::uint32_t offset = 4 + 12 * static_cast<std::uint32_t>(vocs.size());
  for (const auto& voc : vocs) {
    u32(2); // VOC
    u32(offset);
    u32(static_cast<std::uint32_t>(voc.size()));
    offset += static_cast<std::uint32_t>(voc.size());
  }
  for (const auto& voc : vocs) {
    m.insert(m.end(), voc.begin(), voc.end());
  }
  return m;
}

inline std::vector<std::uint8_t> constant_effect_m(std::uint8_t level) {
  return effects_m({level});
}

// An AudioManager on the dummy driver whose device is paused, so the test
// thread is the only one that mixes (see AudioManager::fill_block()).
struct PausedAudio {
  DummyAudio sdl;
  audio::AudioManager am;
  bool ok = false;
  PausedAudio() {
    if (!sdl.ok()) {
      return;
    }
    am.init();
    ok = am.has_device() && SDL_PauseAudioDevice(am.device());
  }
};

inline std::vector<std::int16_t> fill(audio::AudioManager& am) {
  std::vector<std::int16_t> out(audio::MIX_BLOCK_SAMPLES, std::int16_t{0});
  am.fill_block(out);
  return out;
}

// The output level of a constant effect sample value at the default
// volumes, as the Mixer makes it.
inline std::int16_t effect_level(std::int16_t value) {
  audio::Mixer m;
  const auto slot =
      m.add_effect(std::vector<std::int16_t>(audio::MIX_BLOCK_SAMPLES, value));
  m.play(*slot, true);
  std::vector<std::int16_t> out(audio::MIX_BLOCK_SAMPLES);
  m.render(out);
  m.render(out);
  return out.front();
}

#if PLEGACY_ENABLE_MUSIC
// The FM music as AudioManager made it before the music route existed:
// libADLMIDI set up the same way, the same boost, the same mixer. Effects,
// when given, play along from the first block, as in the game's mixer.
class FmReference {
public:
  explicit FmReference(const std::vector<std::uint8_t>& song)
      : player_(adl_init(44100)) {
    adl_setVolumeRangeModel(player_, ADLMIDI_VolumeModel_CMF);
    adl_setNumChips(player_, 1);
    adl_reset(player_);
    adl_openData(player_, song.data(), static_cast<unsigned long>(song.size()));
  }
  ~FmReference() { adl_close(player_); }
  FmReference(const FmReference&) = delete;
  FmReference& operator=(const FmReference&) = delete;

  // A looping constant effect in the reference mix.
  void effect(std::int16_t value) {
    const auto slot = mixer_.add_effect(
        std::vector<std::int16_t>(audio::MIX_BLOCK_SAMPLES, value));
    mixer_.play(*slot, true);
  }

  // The next music block, as render_music() makes it.
  std::vector<std::int16_t> music() {
    std::vector<std::int16_t> b(audio::MIX_BLOCK_SAMPLES, std::int16_t{0});
    const int count = static_cast<int>(b.size());
    const int got = std::max(adl_play(player_, count, b.data()), 0);
    for (int i = 0; i < got; ++i) {
      const auto at = static_cast<std::size_t>(i);
      b[at] = static_cast<std::int16_t>(
          std::clamp(static_cast<float>(b[at]) * 6.0f, -32768.0f, 32767.0f));
    }
    return b;
  }
  // The next output block.
  std::vector<std::int16_t> output() {
    std::vector<std::int16_t> m = music();
    std::vector<std::int16_t> out(audio::MIX_BLOCK_SAMPLES);
    mixer_.render(out, m);
    return out;
  }
  // The next output block with the FM player not run.
  std::vector<std::int16_t> output_without_music() {
    std::vector<std::int16_t> out(audio::MIX_BLOCK_SAMPLES);
    mixer_.render(out);
    return out;
  }

private:
  ADL_MIDIPlayer* player_;
  audio::Mixer mixer_;
};
#endif

} // namespace music_test
