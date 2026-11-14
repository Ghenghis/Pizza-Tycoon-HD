/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Sound effect replacement by event: synthetic WAV overrides through the
// real loader and the real mixer, on the paused dummy device.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "core/content/Sha256.h"
#include "engine/audio/AudioManager.h"
#include "engine/audio/EffectPack.h"

using audio::AudioManager;
using audio::EffectsSet;
using audio::SoundId;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string sha_of(const fs::path& file) {
  const auto bytes = music_test::read_file(file);
  return core::content::Sha256::hex(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// A stereo WAV of one constant sample value, or of a first then a second
// value split at `split` seconds.
fs::path level_wav(const fs::path& file, double seconds, double first,
                   double second = -2.0, double split = 0.0) {
  const auto frames = static_cast<std::size_t>(seconds * 44100);
  std::vector<double> samples(frames * 2, first);
  if (second > -2.0) {
    for (std::size_t f = static_cast<std::size_t>(split * 44100); f < frames;
         ++f) {
      samples[f * 2] = samples[f * 2 + 1] = second;
    }
  }
  music_test::write_file(file, music_test::wav_file({}, samples));
  return file;
}

// What the effect slot sounds like now: its constant output level.
std::int16_t heard(AudioManager& am, SoundId id) {
  am.play_sound(id, audio::LoopMode::Loop);
  music_test::fill(am);
  const auto out = music_test::fill(am);
  am.stop_sound(id);
  music_test::fill(am);
  return out[out.size() / 2];
}

json entry(const std::string& event, const std::string& file,
           const std::string& sha, const std::string& source) {
  return {{"event", event},          {"file", file},   {"sha256", sha},
          {"source_sha256", source}, {"gain_db", 0.0}, {"fallback", "original"},
          {"origin", "authored"}};
}

void write_manifest(const fs::path& folder, const json& effects) {
  const json doc = {{"format", "pthd-sfx-overrides"},
                    {"version", 1},
                    {"pack", {{"id", "test-pack"}, {"revision", 3}}},
                    {"effects", effects}};
  const std::string text = doc.dump(2);
  music_test::write_file(
      folder / "manifest.json",
      std::span(reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size()));
}

const audio::EffectOutcome* outcome(const AudioManager::EffectsChange& c,
                                    const std::string& event) {
  for (const auto& o : c.outcomes) {
    if (o.event == event) {
      return &o;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("event ids keep the 23 effect slots and their numbers",
          "[audio][sfx]") {
  CHECK(static_cast<int>(SoundId::COUNT) == 23);
  CHECK(audio::effect_event_id(SoundId::RESTAURANT_NOISE_1) ==
        "restaurant-noise-1");
  CHECK(audio::effect_event_id(SoundId::PHONE_RINGING) == "phone-ringing");
  CHECK(audio::effect_event_id(SoundId::CHOPPER) == "chopper");
  CHECK(audio::effect_event_id(SoundId::POLICE_SIREN) == "police-siren");
  for (int i = 0; i < static_cast<int>(SoundId::COUNT); ++i) {
    const auto id = static_cast<SoundId>(i);
    CHECK(audio::effect_from_event(audio::effect_event_id(id)) == id);
  }
  CHECK_FALSE(audio::effect_from_event("door-slam"));
}

TEST_CASE("an enhanced pack replaces events, and every refused entry falls "
          "back to its original without moving another slot",
          "[audio][sfx]") {
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  test_support::TempDir dir;
  // Ten originals, each a different constant level.
  const std::vector<std::uint8_t> levels = {132, 136, 140, 144, 148,
                                            152, 156, 160, 164, 168};
  music_test::write_file(dir.path() / "fx.m", music_test::effects_m(levels));
  am.load_m_container((dir.path() / "fx.m").string());
  std::vector<std::int16_t> original;
  for (std::size_t i = 0; i < levels.size(); ++i) {
    const auto id = static_cast<SoundId>(i);
    REQUIRE(am.effect_source_sha256(id).size() == 64);
    original.push_back(heard(am, id));
    CHECK(original.back() == music_test::effect_level(static_cast<std::int16_t>(
                                 (levels[i] - 128) * 256)));
  }
  CHECK(am.effect_source_sha256(SoundId::PHONE_RINGING).empty());
  const auto source = [&](int i) {
    return am.effect_source_sha256(static_cast<SoundId>(i));
  };

  const fs::path pack = dir.path() / "pack";
  const fs::path good = level_wav(pack / "crowd" / "one.wav", 0.5, 0.25);
  const fs::path garbage = pack / "broken.wav";
  music_test::write_file(garbage, std::vector<std::uint8_t>(500, 'g'));
  const fs::path escape = level_wav(dir.path() / "outside.wav", 0.5, 0.1);
  // Over two minutes: mono 8 kHz 8-bit keeps it small on disk.
  const fs::path longer = pack / "long.wav";
  music_test::write_file(
      longer, music_test::wav_file(
                  {.format = 1, .channels = 1, .rate = 8000, .bits = 8},
                  std::vector<double>(8000 * 121, 0.1)));
  const fs::path split =
      level_wav(pack / "trimmed.wav", 1.0, 0.0625, 0.1875, 0.5);
  write_manifest(
      pack,
      json::array({
          entry("restaurant-noise-1", "crowd/one.wav", sha_of(good), source(0)),
          entry("restaurant-noise-2", "crowd/gone.wav", sha_of(good),
                source(1)),
          entry("restaurant-noise-3", "broken.wav", sha_of(garbage), source(2)),
          entry("traffic-1", "../outside.wav", sha_of(escape), source(3)),
          entry("traffic-2", "long.wav", sha_of(longer), source(4)),
          entry("dial-tone-0", "crowd/one.wav", std::string(64, 'a'),
                source(5)),
          entry("dial-tone-1", "crowd/one.wav", sha_of(good), source(0)),
          [&] {
            json e =
                entry("dial-tone-2", "trimmed.wav", sha_of(split), source(7));
            e["trim_start_ms"] = 500;
            return e;
          }(),
          [&] {
            json e =
                entry("dial-tone-3", "crowd/one.wav", sha_of(good), source(8));
            e["slot"] = 99;
            return e;
          }(),
          entry("dial-tone-4", music::utf8_path(good), sha_of(good), source(9)),
          entry("door-slam", "crowd/one.wav", sha_of(good), source(0)),
      }));

  am.effects_pack(pack);
  const auto done = am.select_effects(EffectsSet::ENHANCED);
  INFO(done.error);
  REQUIRE(done);
  CHECK(am.effects() == EffectsSet::ENHANCED);
  REQUIRE(done.outcomes.size() == 11);
  const auto reason = [&](const std::string& event) {
    const auto* o = outcome(done, event);
    return o ? o->reason : std::string("no outcome");
  };
  CHECK(outcome(done, "restaurant-noise-1")->replaced);
  CHECK(outcome(done, "dial-tone-2")->replaced);
  CHECK(reason("restaurant-noise-2").find("missing") != std::string::npos);
  CHECK_FALSE(reason("restaurant-noise-3").empty()); // the decoder's reason
  CHECK(reason("traffic-1").find("outside the pack") != std::string::npos);
  CHECK(reason("traffic-2").find("longer than") != std::string::npos);
  CHECK(reason("dial-tone-0").find("sha256") != std::string::npos);
  CHECK(reason("dial-tone-1").find("different original") != std::string::npos);
  CHECK(reason("dial-tone-3").find("slot") != std::string::npos);
  CHECK(reason("dial-tone-4").find("inside the pack") != std::string::npos);
  CHECK(reason("door-slam").find("no such event") != std::string::npos);
  CHECK(outcome(done, "door-slam")->slot == -1);

  // What plays: the replacement; its trims are its own (only the second
  // half); every other slot its original, in its own place.
  CHECK(heard(am, SoundId::RESTAURANT_NOISE_1) ==
        music_test::effect_level(
            static_cast<std::int16_t>(std::lround(0.25 * 32767))));
  CHECK(heard(am, SoundId::DIAL_TONE_2) ==
        music_test::effect_level(
            static_cast<std::int16_t>(std::lround(0.1875 * 32767))));
  for (const int i : {1, 2, 3, 4, 5, 6, 8, 9}) {
    CHECK(heard(am, static_cast<SoundId>(i)) ==
          original[static_cast<std::size_t>(i)]);
  }

  // A pack that cannot be used keeps the working selection as it was.
  const fs::path bad = dir.path() / "bad";
  music_test::write_file(bad / "manifest.json",
                         std::vector<std::uint8_t>(20, '{'));
  am.effects_pack(bad);
  const auto refused = am.select_effects(EffectsSet::ENHANCED);
  CHECK_FALSE(refused);
  CHECK(refused.error.find("damaged") != std::string::npos);
  CHECK(am.effects() == EffectsSet::ENHANCED);
  CHECK(heard(am, SoundId::RESTAURANT_NOISE_1) ==
        music_test::effect_level(
            static_cast<std::int16_t>(std::lround(0.25 * 32767))));

  // Original again, live; then a missing pack changes nothing.
  REQUIRE(am.select_effects(EffectsSet::ORIGINAL));
  CHECK(heard(am, SoundId::RESTAURANT_NOISE_1) == original[0]);
  am.effects_pack(dir.path() / "not-there");
  CHECK_FALSE(am.select_effects(EffectsSet::ENHANCED));
  CHECK(am.effects() == EffectsSet::ORIGINAL);
  CHECK(heard(am, SoundId::DIAL_TONE_2) == original[7]);
}

TEST_CASE("a looping effect plays on through a switch of effect sets",
          "[audio][sfx]") {
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  test_support::TempDir dir;
  music_test::write_file(dir.path() / "fx.m", music_test::effects_m({132}));
  am.load_m_container((dir.path() / "fx.m").string());
  const fs::path pack = dir.path() / "pack";
  const fs::path wav = level_wav(pack / "loop.wav", 0.2, 0.5);
  write_manifest(
      pack, json::array(
                {entry("restaurant-noise-1", "loop.wav", sha_of(wav),
                       am.effect_source_sha256(SoundId::RESTAURANT_NOISE_1))}));
  am.effects_pack(pack);
  am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  const std::int16_t before = music_test::fill(am).back();
  REQUIRE(am.select_effects(EffectsSet::ENHANCED));
  // Switched in the very next block, without a restart or a gap; the
  // replacement is shorter than the original and loops by itself.
  for (int i = 0; i < 20; ++i) {
    const auto out = music_test::fill(am);
    CHECK(out.front() == music_test::effect_level(static_cast<std::int16_t>(
                             std::lround(0.5 * 32767))));
  }
  REQUIRE(am.select_effects(EffectsSet::ORIGINAL));
  CHECK(music_test::fill(am).back() == before);
  CHECK(am.is_playing());
}

TEST_CASE("an enhanced pack chosen at startup and then refused is reported "
          "once, and the originals play",
          "[audio][sfx]") {
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  test_support::TempDir dir;
  const std::vector<std::uint8_t> levels = {132, 136, 140};
  music_test::write_file(dir.path() / "fx.m", music_test::effects_m(levels));
  const fs::path bad = dir.path() / "bad";
  music_test::write_file(bad / "manifest.json",
                         std::vector<std::uint8_t>(20, '{'));

  // As Game does before the assets load: the choice waits for them.
  am.effects_pack(bad);
  CHECK(am.select_effects(EffectsSet::ENHANCED));
  CHECK(am.take_effects_problem().empty());

  am.load_m_container((dir.path() / "fx.m").string());
  am.effects_loaded();
  const std::string problem = am.take_effects_problem();
  CHECK(problem.find("damaged") != std::string::npos);
  CHECK(am.take_effects_problem().empty());
  CHECK(am.effects() == EffectsSet::ORIGINAL);
  CHECK(heard(am, SoundId::RESTAURANT_NOISE_1) ==
        music_test::effect_level(static_cast<std::int16_t>(4 * 256)));

  // Refused live, the menu has the reason from select_effects(); nothing
  // is kept for later.
  am.effects_pack(dir.path() / "not-there");
  CHECK_FALSE(am.select_effects(EffectsSet::ENHANCED));
  CHECK(am.take_effects_problem().empty());
}
