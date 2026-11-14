/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "engine/music/TrackDecoder.h"

using Catch::Approx;
using music::Limits;
using music::TrackReader;
using music_test::WavSpec;
namespace fs = std::filesystem;

namespace {

struct Decoded {
  std::vector<std::int16_t> samples; // stereo, 44100 Hz
  bool damaged = false;
  bool ended = false;
};

// Reads the whole track through the decoder and SDL's converter.
Decoded decode_all(TrackReader& reader, std::size_t max_frames = 44100 * 10) {
  Decoded d;
  std::vector<std::int16_t> chunk(1024 * 2);
  while (d.samples.size() / 2 < max_frames) {
    const std::size_t got = reader.read(chunk);
    d.samples.insert(d.samples.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
    if (got == 0) {
      break;
    }
  }
  d.ended = reader.at_end();
  d.damaged = reader.damaged();
  return d;
}

Decoded decode_file(const fs::path& file, const Limits& limits = {}) {
  std::string error;
  auto decoder = music::open_builtin_decoder(file, limits, &error);
  INFO(error);
  REQUIRE(decoder);
  TrackReader reader;
  REQUIRE(reader.open(std::move(decoder), &error));
  return decode_all(reader);
}

// The reason a file is refused; empty when it is not.
std::string refusal(const fs::path& file, const Limits& limits = {}) {
  std::string error;
  auto decoder = music::open_builtin_decoder(file, limits, &error);
  return decoder ? std::string{} : error;
}

bool mentions(const std::string& text, const std::string& word) {
  return text.find(word) != std::string::npos;
}

} // namespace

TEST_CASE("WAV files in every supported encoding decode to the game's format",
          "[music][decoder]") {
  test_support::TempDir dir;
  struct Case {
    WavSpec spec;
    double hz;
  };
  const Case cases[] = {
      {{.format = 1, .channels = 1, .rate = 22050, .bits = 8}, 440.0},
      {{.format = 1, .channels = 2, .rate = 44100, .bits = 16}, 440.0},
      {{.format = 1, .channels = 2, .rate = 48000, .bits = 24}, 550.0},
      {{.format = 1, .channels = 1, .rate = 32000, .bits = 32}, 330.0},
      {{.format = 3, .channels = 2, .rate = 44100, .bits = 32}, 440.0},
      {{.format = 3, .channels = 1, .rate = 16000, .bits = 64}, 220.0},
      {{.format = 1, .channels = 6, .rate = 44100, .bits = 16}, 440.0},
  };
  int n = 0;
  for (const Case& c : cases) {
    const fs::path file = music_test::tone_wav(
        dir.path() / ("tone" + std::to_string(n++) + ".wav"), c.hz, 0.5,
        c.spec);
    INFO("channels " << c.spec.channels << ", " << c.spec.rate << " Hz, "
                     << c.spec.bits << " bits, format " << c.spec.format);
    std::string error;
    const auto info = music::probe_file(file, {}, &error);
    REQUIRE(info);
    CHECK(info->format == music::Format::WAV);
    CHECK(info->channels == c.spec.channels);
    CHECK(info->sample_rate == c.spec.rate);
    CHECK(info->frames == c.spec.rate / 2);
    CHECK(info->frames_exact);

    const Decoded d = decode_file(file);
    CHECK(d.ended);
    CHECK_FALSE(d.damaged);
    const double frames = static_cast<double>(d.samples.size() / 2);
    CHECK(frames == Approx(22050.0).margin(64));
    CHECK(music_test::frequency(d.samples, 0) == Approx(c.hz).epsilon(0.03));
    CHECK(music_test::frequency(d.samples, 1) == Approx(c.hz).epsilon(0.03));
    // Mono plays on both sides; surround is mixed down, not dropped.
    CHECK(music_test::peak(d.samples, 0) > 3000);
    CHECK(music_test::peak(d.samples, 1) > 3000);
  }
}

TEST_CASE("WAV tags and a Unicode file name", "[music][decoder]") {
  test_support::TempDir dir;
  const fs::path file =
      dir.path() / music_test::u8(u8"Caf\u00e9 \u266a 01.wav");
  const WavSpec spec;
  // INFO text is often Latin-1: "Caf\xe9" is read as "Caf\u00e9".
  music_test::write_file(
      file,
      music_test::wav_file(spec, music_test::tone(440, 4410, 2, 44100),
                           {{"LIST", music_test::info_title("Caf\xe9")}}));
  std::string error;
  const auto info = music::probe_file(file, {}, &error);
  INFO(error);
  REQUIRE(info);
  CHECK(info->title == "Caf\xc3\xa9");
  const Decoded d = decode_file(file);
  CHECK(d.samples.size() / 2 == Approx(4410).margin(32));
}

TEST_CASE("damaged and unsupported files are refused with a reason before "
          "any decoder runs",
          "[music][decoder]") {
  test_support::TempDir dir;
  const WavSpec spec;
  const auto good =
      music_test::wav_file(spec, music_test::tone(440, 44100, 2, 44100));
  const auto put = [&](const std::string& name,
                       std::vector<std::uint8_t> bytes) {
    const fs::path p = dir.path() / name;
    music_test::write_file(p, bytes);
    return p;
  };

  // Cut short: the data chunk promises more than the file holds.
  auto cut = good;
  cut.resize(cut.size() / 2);
  CHECK(mentions(refusal(put("cut.wav", cut)), "cut short"));

  std::mt19937 rng(5);
  std::vector<std::uint8_t> noise(4096);
  for (auto& b : noise) {
    b = static_cast<std::uint8_t>(rng() & 0x7F); // never an MPEG sync
  }
  CHECK(
      mentions(refusal(put("noise.wav", noise)), "not a supported audio file"));

  // A format chunk claiming two gigabytes.
  auto huge_fmt = good;
  huge_fmt[16] = 0xF0;
  huge_fmt[17] = 0xFF;
  huge_fmt[18] = 0xFF;
  huge_fmt[19] = 0x7F;
  CHECK(mentions(refusal(put("fmt.wav", huge_fmt)), "format chunk"));

  // 8 KiB of header before the audio, over a 4 KiB limit.
  Limits small_header;
  small_header.max_header_bytes = 4096;
  const auto padded =
      music_test::wav_file(spec, music_test::tone(440, 4410, 2, 44100),
                           {{"JUNK", std::vector<std::uint8_t>(8192, 0)}});
  CHECK(
      mentions(refusal(put("header.wav", padded), small_header), "too large"));

  CHECK(mentions(
      refusal(put("nine.wav",
                  music_test::wav_file({.channels = 9},
                                       music_test::tone(440, 441, 9, 44100)))),
      "9 channels"));
  CHECK(mentions(
      refusal(put("slow.wav",
                  music_test::wav_file({.rate = 4000},
                                       music_test::tone(440, 400, 2, 4000)))),
      "sample rate"));
  CHECK(mentions(refusal(put("empty-data.wav", music_test::wav_file(spec, {}))),
                 "no audio"));
  Limits one_second;
  one_second.max_seconds = 1;
  CHECK(
      mentions(refusal(music_test::tone_wav(dir.path() / "long.wav", 440, 2.5),
                       one_second),
               "longer than"));
  Limits tiny;
  tiny.max_file_bytes = 100;
  CHECK(mentions(refusal(put("big.wav", good), tiny), "larger than"));

  auto adpcm = good;
  adpcm[20] = 2; // MS ADPCM
  CHECK(
      mentions(refusal(put("adpcm.wav", adpcm)), "encoding is not supported"));
  auto odd_align = good;
  odd_align[32] = 3; // block align that does not match 2 x 16 bits
  CHECK(mentions(refusal(put("align.wav", odd_align)), "does not add up"));
  auto rifx = good;
  rifx[3] = 'X';
  CHECK(mentions(refusal(put("rifx.wav", rifx)), "not supported"));

  std::vector<std::uint8_t> flac = {'f', 'L', 'a', 'C', 0, 0,
                                    0,   34,  0,   0,   0, 0};
  CHECK(mentions(refusal(put("song.flac", flac)), "FLAC"));
  std::vector<std::uint8_t> ogg = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
  CHECK(mentions(refusal(put("song.ogg", ogg)), "Ogg"));
  CHECK(mentions(refusal(put("zero.wav", {})), "empty"));
  CHECK(mentions(refusal(dir.path() / "not-there.mp3"), "cannot be opened"));
}

TEST_CASE("the MP3 fixtures decode with their tags, length and pitch",
          "[music][decoder][mp3]") {
  SECTION("stereo, 44100 Hz, with a LAME Info header and ID3v2.3 tags") {
    const fs::path file = music_test::fixture("tone-440-stereo-44k.mp3");
    std::string error;
    const auto info = music::probe_file(file, {}, &error);
    INFO(error);
    REQUIRE(info);
    CHECK(info->format == music::Format::MP3);
    CHECK(info->sample_rate == 44100);
    CHECK(info->channels == 2);
    CHECK(info->bitrate_kbps == 96);
    CHECK(info->encoding == "MPEG-1 Layer III");
    CHECK(info->frames_exact);
    CHECK(info->title == "Caf\xc3\xa9 \xe2\x99\xaa 01"); // UTF-16 in the tag
    CHECK(info->artist == "Pizza Tycoon HD tests");
    CHECK(info->album == "Tones");

    auto decoder = music::open_builtin_decoder(file, {}, &error);
    REQUIRE(decoder);
    CHECK(decoder->info().frames == Approx(44100).margin(1152));
    CHECK(decoder->backend().starts_with("dr_mp3 0.7.4"));
    TrackReader reader;
    REQUIRE(reader.open(std::move(decoder), &error));
    const Decoded d = decode_all(reader);
    CHECK(d.ended);
    CHECK_FALSE(d.damaged);
    CHECK(static_cast<double>(d.samples.size() / 2) ==
          Approx(44100).margin(1152));
    CHECK(music_test::frequency(d.samples) == Approx(440).epsilon(0.03));
    // Once at the end, it stays there.
    std::vector<std::int16_t> more(512);
    CHECK(reader.read(more) == 0);
    CHECK(reader.at_end());
  }
  SECTION("mono MPEG-2 at 22050 Hz without a Xing header, ID3v2.4 tags") {
    const fs::path file = music_test::fixture("tone-660-mono-22k.mp3");
    std::string error;
    const auto info = music::probe_file(file, {}, &error);
    INFO(error);
    REQUIRE(info);
    CHECK(info->sample_rate == 22050);
    CHECK(info->channels == 1);
    CHECK(info->encoding == "MPEG-2 Layer III");
    CHECK_FALSE(info->frames_exact); // estimated from the bitrate
    CHECK(static_cast<double>(info->frames) ==
          Approx(1.5 * 22050).epsilon(0.08));
    CHECK(info->title == "Tone 660");
    const Decoded d = decode_file(file);
    CHECK(d.ended);
    // Resampled to 44100 Hz; the encoder's delay is in there too.
    CHECK(static_cast<double>(d.samples.size() / 2) ==
          Approx(1.5 * 44100).epsilon(0.06));
    CHECK(music_test::frequency(d.samples, 0) == Approx(660).epsilon(0.03));
    CHECK(music_test::peak(d.samples, 1) > 1000); // mono on both sides
  }
}

TEST_CASE("seeking lands where asked, in WAV exactly and in MP3 closely",
          "[music][decoder]") {
  test_support::TempDir dir;
  const fs::path wav = music_test::tone_wav(dir.path() / "seek.wav", 440, 1.0);
  const Decoded whole = decode_file(wav);
  std::string error;
  {
    TrackReader reader;
    REQUIRE(reader.open(music::open_builtin_decoder(wav, {}, &error), &error));
    REQUIRE(reader.seek(22050));
    CHECK(reader.position() == 22050);
    std::vector<std::int16_t> chunk(1000 * 2);
    REQUIRE(reader.read(chunk) == 1000);
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      REQUIRE(std::abs(chunk[i] - whole.samples[22050 * 2 + i]) <= 1);
    }
    // Back to the start, and past the end.
    REQUIRE(reader.seek(0));
    REQUIRE(reader.read(chunk) == 1000);
    CHECK(chunk[2] == whole.samples[2]);
    REQUIRE(reader.seek(10 * 44100));
    const Decoded rest = decode_all(reader);
    CHECK(rest.samples.empty());
    CHECK(rest.ended);
  }
  {
    const fs::path mp3 = music_test::fixture("tone-440-stereo-44k.mp3");
    TrackReader reader;
    REQUIRE(reader.open(music::open_builtin_decoder(mp3, {}, &error), &error));
    REQUIRE(reader.seek(22050));
    const Decoded rest = decode_all(reader);
    CHECK(rest.ended);
    CHECK_FALSE(rest.damaged);
    CHECK(static_cast<double>(rest.samples.size() / 2) ==
          Approx(22050).margin(1152));
    CHECK(music_test::frequency(rest.samples) == Approx(440).epsilon(0.03));
  }
}

TEST_CASE("damaged MP3s: refused when no audio is found, flagged when they "
          "end early",
          "[music][decoder][mp3]") {
  test_support::TempDir dir;
  const auto bytes =
      music_test::read_file(music_test::fixture("tone-440-stereo-44k.mp3"));
  REQUIRE(bytes.size() > 4000);

  // Cut at 40 percent: its Info header still promises the full length.
  auto cut = bytes;
  cut.resize(bytes.size() * 4 / 10);
  const fs::path cut_file = dir.path() / "cut.mp3";
  music_test::write_file(cut_file, cut);
  const Decoded d = decode_file(cut_file);
  CHECK(d.ended);
  CHECK(d.damaged);
  CHECK(static_cast<double>(d.samples.size() / 2) < 0.6 * 44100);

  // The tags, then noise: no MPEG frame anywhere.
  std::vector<std::uint8_t> noise(bytes.begin(), bytes.begin() + 103);
  std::mt19937 rng(7);
  for (int i = 0; i < 6000; ++i) {
    noise.push_back(static_cast<std::uint8_t>(rng() & 0x7F));
  }
  const fs::path noise_file = dir.path() / "noise.mp3";
  music_test::write_file(noise_file, noise);
  CHECK(mentions(refusal(noise_file), "no audio frames"));

  // Noise in the middle: the decoder finds its way back and the track
  // still ends.
  auto holed = bytes;
  for (std::size_t i = bytes.size() / 2; i < bytes.size() / 2 + 900; ++i) {
    holed[i] = static_cast<std::uint8_t>(rng());
  }
  const fs::path holed_file = dir.path() / "holed.mp3";
  music_test::write_file(holed_file, holed);
  const Decoded h = decode_file(holed_file);
  CHECK(h.ended);
  CHECK(h.samples.size() / 2 > 22050);
}
