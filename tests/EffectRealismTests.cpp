/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The effects' realism (docs/studio/notes/sfx-overrides.md, version 2):
// variant pools, layered ambience with a voice limit and priorities,
// placement on the screen and music ducking. Every sound is a real file,
// read and decoded by the real loader, and heard through the real mixer.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <set>
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

// A 16-bit stereo WAV of one constant level (-1..1).
fs::path level_wav(const fs::path& file, double seconds, double level) {
  const auto frames = static_cast<std::size_t>(seconds * 44100);
  music_test::write_file(
      file, music_test::wav_file({}, std::vector<double>(frames * 2, level)));
  return file;
}

// The int16 value a level is stored as in those files.
std::int16_t stored(double level) {
  return static_cast<std::int16_t>(std::lround(level * 32767));
}

json sound(const fs::path& pack, const fs::path& file) {
  return {{"file", file.lexically_relative(pack).generic_string()},
          {"sha256", sha_of(file)}};
}

void write_manifest(const fs::path& pack, int version, const json& effects,
                    const json& ambience = json::array()) {
  json doc = {{"format", "pthd-sfx-overrides"},
              {"version", version},
              {"pack", {{"id", "realism-test"}, {"revision", 1}}},
              {"effects", effects}};
  if (!ambience.empty()) {
    doc["ambience"] = ambience;
  }
  const std::string text = doc.dump(2);
  music_test::write_file(
      pack / "manifest.json",
      std::span(reinterpret_cast<const std::uint8_t*>(text.data()),
                text.size()));
}

// The originals: sixteen constant levels, so the phone (slot 15) is there.
struct Game {
  music_test::PausedAudio a;
  test_support::TempDir dir;
  fs::path pack;
  std::vector<std::uint8_t> levels;
  bool ok = false;

  Game() {
    if (!a.ok) {
      return;
    }
    for (int i = 0; i < 16; ++i) {
      levels.push_back(static_cast<std::uint8_t>(130 + i));
    }
    music_test::write_file(dir.path() / "fx.m", music_test::effects_m(levels));
    a.am.load_m_container((dir.path() / "fx.m").string());
    a.am.effects_seed(42);
    pack = dir.path() / "pack";
    a.am.effects_pack(pack);
    ok = true;
  }
  [[nodiscard]] std::int16_t original(std::size_t slot) const {
    return static_cast<std::int16_t>((levels[slot] - 128) * 256);
  }
  [[nodiscard]] std::string source(SoundId id) const {
    return a.am.effect_source_sha256(id);
  }
};

// Mid-block output of the next block.
std::int16_t level_now(AudioManager& am) {
  const auto out = music_test::fill(am);
  return out[out.size() / 2];
}

bool near(int heard, int expected) { return std::abs(heard - expected) <= 1; }

const audio::EffectOutcome* outcome_of(const AudioManager::EffectsChange& c,
                                       const std::string& event,
                                       const std::string& layer = {}) {
  for (const auto& o : c.outcomes) {
    if (o.event == event && o.layer == layer) {
      return &o;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("a variant pool plays one of its sounds each time, never the same "
          "twice running",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const std::vector<double> variant_levels = {0.1, 0.2, 0.3};
  json variants = json::array();
  for (std::size_t i = 0; i < variant_levels.size(); ++i) {
    variants.push_back(sound(
        g.pack, level_wav(g.pack / "door" / ("v" + std::to_string(i) + ".wav"),
                          0.5, variant_levels[i])));
  }
  write_manifest(
      g.pack, 2,
      json::array({{{"event", "restaurant-noise-2"},
                    {"source_sha256", g.source(SoundId::RESTAURANT_NOISE_2)},
                    {"variants", variants}}}));
  const auto done = am.select_effects(EffectsSet::ENHANCED);
  REQUIRE(done);
  const auto* o = outcome_of(done, "restaurant-noise-2");
  REQUIRE(o);
  CHECK(o->replaced);
  CHECK(o->variants == 3);
  CHECK(o->reason.empty());

  std::vector<std::int16_t> expected;
  for (const double v : variant_levels) {
    expected.push_back(music_test::effect_level(stored(v)));
  }
  std::vector<std::size_t> heard;
  for (int k = 0; k < 40; ++k) {
    am.play_sound(SoundId::RESTAURANT_NOISE_2, audio::LoopMode::Loop);
    const std::int16_t level = level_now(am);
    am.stop_sound(SoundId::RESTAURANT_NOISE_2);
    music_test::fill(am);
    const auto it = std::find(expected.begin(), expected.end(), level);
    REQUIRE(it != expected.end()); // always one of the decoded files
    heard.push_back(static_cast<std::size_t>(it - expected.begin()));
    CHECK(heard.back() == am.mixer().last_variant(1));
  }
  for (std::size_t k = 1; k < heard.size(); ++k) {
    CHECK(heard[k] != heard[k - 1]);
  }
  CHECK(std::set(heard.begin(), heard.end()).size() == 3);

  // The same seed, the same choices: the generator is the effects' own.
  std::vector<std::size_t> again;
  am.effects_seed(42);
  REQUIRE(am.select_effects(EffectsSet::ENHANCED));
  for (int k = 0; k < 40; ++k) {
    am.play_sound(SoundId::RESTAURANT_NOISE_2, audio::LoopMode::Loop);
    again.push_back(am.mixer().last_variant(1));
    am.stop_sound(SoundId::RESTAURANT_NOISE_2);
    music_test::fill(am);
  }
  CHECK(again == heard);
}

TEST_CASE("version 2 refuses only a broken variant; version 1 reads as it "
          "always did; a later version is refused",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const fs::path a = level_wav(g.pack / "a.wav", 0.5, 0.25);
  const fs::path b = level_wav(g.pack / "b.wav", 0.5, 0.5);
  json broken = sound(g.pack, b);
  broken["sha256"] = std::string(64, 'a');
  const json entry = {{"event", "traffic-1"},
                      {"source_sha256", g.source(SoundId::TRAFFIC_1)},
                      {"file", "a.wav"},
                      {"sha256", sha_of(a)},
                      {"priority", "alert"},
                      {"variants", json::array({sound(g.pack, a), broken})}};

  SECTION("version 2") {
    write_manifest(g.pack, 2, json::array({entry}));
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    REQUIRE(done);
    const auto* o = outcome_of(done, "traffic-1");
    REQUIRE(o);
    CHECK(o->replaced);
    CHECK(o->variants == 1);
    CHECK(o->reason.find("variant 2: its file does not match its sha256") !=
          std::string::npos);
    // Its priority is the pack's.
    CHECK(am.mixer().effect_bank()->priorities[3] == audio::Priority::ALERT);
    am.play_sound(SoundId::TRAFFIC_1, audio::LoopMode::Loop);
    CHECK(level_now(am) == music_test::effect_level(stored(0.25)));
  }
  SECTION("version 1 ignores the version 2 fields") {
    write_manifest(g.pack, 1, json::array({entry}));
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    REQUIRE(done);
    const auto* o = outcome_of(done, "traffic-1");
    REQUIRE(o);
    CHECK(o->replaced);
    CHECK(o->variants == 1);
    CHECK(o->reason.empty());
    CHECK_FALSE(am.mixer().effect_bank()->priorities[3]);
  }
  SECTION("a bad priority refuses the entry") {
    json bad = entry;
    bad["priority"] = "urgent";
    write_manifest(g.pack, 2, json::array({bad}));
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    REQUIRE(done);
    const auto* o = outcome_of(done, "traffic-1");
    REQUIRE(o);
    CHECK_FALSE(o->replaced);
    CHECK(o->reason.find("priority") != std::string::npos);
  }
  SECTION("version 3") {
    write_manifest(g.pack, 3, json::array({entry}));
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    CHECK_FALSE(done);
    CHECK(done.error.find("version") != std::string::npos);
    CHECK(am.effects() == EffectsSet::ORIGINAL);
  }
}

TEST_CASE("ambience one-shots come at their interval only while their bed "
          "plays, on top of it, and end with their pack",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  // The bed is the original restaurant noise; the cutlery is the pack's.
  const fs::path cutlery = level_wav(g.pack / "cutlery.wav", 0.2, 0.25);
  const json ambience = json::array(
      {{{"with", "restaurant-noise-1"},
        {"layers",
         json::array({{{"name", "cutlery"},
                       {"variants", json::array({sound(g.pack, cutlery)})},
                       {"interval_ms", json::array({300, 300})}},
                      {{"name", "too fast"},
                       {"variants", json::array({sound(g.pack, cutlery)})},
                       {"interval_ms", json::array({100, 200})}}})}}});
  write_manifest(g.pack, 2, json::array(), ambience);
  const auto done = am.select_effects(EffectsSet::ENHANCED);
  REQUIRE(done);
  const auto* layer = outcome_of(done, "restaurant-noise-1", "cutlery");
  REQUIRE(layer);
  CHECK(layer->replaced);
  CHECK(layer->variants == 1);
  const auto* fast = outcome_of(done, "restaurant-noise-1", "too fast");
  REQUIRE(fast);
  CHECK_FALSE(fast->replaced);
  CHECK(fast->reason.find("interval_ms") != std::string::npos);

  const std::int16_t bed = music_test::effect_level(g.original(0));
  const std::int16_t both = music_test::effect_level(
      static_cast<std::int16_t>(g.original(0) + stored(0.25)));

  // No bed, no one-shots, however long.
  am.tick(10000);
  music_test::fill(am);
  CHECK(am.mixer().extra_voices() == 0);

  am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  am.tick(0); // the interval starts with the bed
  CHECK(level_now(am) == bed);
  am.tick(299);
  CHECK(level_now(am) == bed);
  CHECK(am.mixer().extra_voices() == 0);
  am.tick(1);
  CHECK(near(level_now(am), both));
  CHECK(am.mixer().extra_voices() == 1);
  // 0.2 s is under nine blocks; then the bed alone again.
  for (int i = 0; i < 9; ++i) {
    music_test::fill(am);
  }
  CHECK(level_now(am) == bed);
  CHECK(am.mixer().extra_voices() == 0);

  // The bed stops: nothing more comes.
  am.stop_sound(SoundId::RESTAURANT_NOISE_1);
  music_test::fill(am);
  am.tick(5000);
  am.tick(5000);
  music_test::fill(am);
  CHECK(am.mixer().extra_voices() == 0);

  // Back to the original effects: the pack's one-shots end with it.
  am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  am.tick(0);
  am.tick(300);
  music_test::fill(am);
  REQUIRE(am.mixer().extra_voices() == 1);
  REQUIRE(am.select_effects(EffectsSet::ORIGINAL));
  CHECK(level_now(am) == bed);
  CHECK(am.mixer().extra_voices() == 0);
  am.tick(1000);
  music_test::fill(am);
  CHECK(am.mixer().extra_voices() == 0);
}

TEST_CASE("at the voice limit ambience waits its turn, and an alert cuts it",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const fs::path chatter = level_wav(g.pack / "chatter.wav", 3.0, 0.25);
  write_manifest(
      g.pack, 2, json::array(),
      json::array(
          {{{"with", "restaurant-noise-1"},
            {"layers",
             json::array({{{"name", "chatter"},
                           {"variants", json::array({sound(g.pack, chatter)})},
                           {"interval_ms", json::array({250, 250})}}})}}}));
  REQUIRE(am.select_effects(EffectsSet::ENHANCED));
  CHECK(audio::default_priority(SoundId::PHONE_RINGING) ==
        audio::Priority::ALERT);
  CHECK(audio::default_priority(SoundId::RESTAURANT_NOISE_1) ==
        audio::Priority::AMBIENCE);
  CHECK(audio::default_priority(SoundId::DIAL_TONE_3) ==
        audio::Priority::FEEDBACK);

  am.voice_limit(2);
  am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  am.tick(0);
  am.tick(250);
  music_test::fill(am);
  REQUIRE(am.mixer().extra_voices() == 1); // bed + chatter: the limit
  const std::uint64_t dropped = am.mixer().dropped_voices();
  am.tick(250); // a second one has no voice
  music_test::fill(am);
  CHECK(am.mixer().extra_voices() == 1);
  CHECK(am.mixer().dropped_voices() == dropped + 1);

  // The phone rings: the chatter makes way, the bed stays.
  am.play_sound(SoundId::PHONE_RINGING, audio::LoopMode::Loop);
  const std::int16_t heard = level_now(am);
  CHECK(am.mixer().extra_voices() == 0);
  CHECK(am.mixer().dropped_voices() == dropped + 2);
  CHECK(near(heard, music_test::effect_level(static_cast<std::int16_t>(
                        g.original(0) + g.original(15)))));
}

TEST_CASE("a placed sound is panned and, off the screen, quieter",
          "[audio][sfx][realism]") {
  const auto mid = audio::screen_placement(320, 200, 640, 400);
  CHECK(mid.pan == 0.0f);
  CHECK(mid.gain == 1.0f);
  CHECK(audio::screen_placement(0, 200, 640, 400).pan == -audio::PAN_LIMIT);
  CHECK(audio::screen_placement(640, 200, 640, 400).pan == audio::PAN_LIMIT);
  CHECK(audio::screen_placement(640, 200, 640, 400).gain == 1.0f);
  const auto far_left = audio::screen_placement(-640, 200, 640, 400);
  CHECK(far_left.pan == -audio::PAN_LIMIT);
  CHECK(std::abs(far_left.gain - audio::OFF_SCREEN_GAIN) < 1e-6f);
  CHECK(std::abs(audio::screen_placement(-320, 200, 640, 400).gain - 0.65f) <
        1e-6f);
  CHECK(audio::screen_placement(-5000, 200, 640, 400).gain ==
        audio::OFF_SCREEN_GAIN);

  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const fs::path door = level_wav(g.pack / "door.wav", 0.5, 0.5);
  write_manifest(
      g.pack, 2,
      json::array({{{"event", "dial-tone-0"},
                    {"source_sha256", g.source(SoundId::DIAL_TONE_0)},
                    {"file", "door.wav"},
                    {"sha256", sha_of(door)}}}));
  REQUIRE(am.select_effects(EffectsSet::ENHANCED));
  const std::int16_t full = music_test::effect_level(stored(0.5));

  am.play_sound_at(SoundId::DIAL_TONE_0, {.pan = 0.5f, .gain = 1.0f},
                   audio::LoopMode::Loop);
  auto out = music_test::fill(am);
  CHECK(near(out[out.size() / 2], full / 2)); // left
  CHECK(out[out.size() / 2 + 1] == full);     // right
  am.stop_sound(SoundId::DIAL_TONE_0);
  music_test::fill(am);

  am.play_sound_at(SoundId::DIAL_TONE_0, {.pan = -1.0f, .gain = 0.5f},
                   audio::LoopMode::Loop);
  out = music_test::fill(am);
  CHECK(near(out[out.size() / 2], full / 2));
  CHECK(out[out.size() / 2 + 1] == 0);
  am.stop_sound(SoundId::DIAL_TONE_0);
  music_test::fill(am);

  // Unplaced, it is as it always was.
  am.play_sound(SoundId::DIAL_TONE_0, audio::LoopMode::Loop);
  out = music_test::fill(am);
  CHECK(out[out.size() / 2] == full);
  CHECK(out[out.size() / 2 + 1] == full);
}

TEST_CASE("music dips under an alert, smoothly, and comes back; ducking off "
          "leaves it",
          "[audio][mix][realism]") {
  audio::Mixer m;
  // The phone: a decoded file would do the same; only its priority counts.
  const auto phone = m.add_effect(std::vector<std::int16_t>(4096, 0));
  const auto tone = m.add_effect(std::vector<std::int16_t>(4096, 0));
  REQUIRE(phone);
  REQUIRE(tone);
  m.priority(*phone, audio::Priority::ALERT);
  m.priority(*tone, audio::Priority::FEEDBACK);
  const std::vector<std::int16_t> music(audio::MIX_BLOCK_SAMPLES, 10000);
  std::vector<std::int16_t> out(audio::MIX_BLOCK_SAMPLES);
  const auto block = [&] {
    m.render(out, music);
    return out;
  };
  const int full = block().back();
  REQUIRE(full == 8000); // 10000 * 0.8 headroom

  // Feedback does not duck.
  m.play(*tone, true);
  CHECK(block().back() == full);
  m.stop(*tone);

  m.play(*phone, true);
  int last = full;
  int blocks = 0;
  while (last > 4500 && blocks < 20) {
    const auto o = block();
    for (std::size_t i = 2; i < o.size(); i += 2) {
      // Never a step: neighbouring frames differ by a few values at most.
      CHECK(std::abs(o[i] - o[i - 2]) <= 3);
      CHECK(o[i] <= last);
    }
    last = o.back();
    ++blocks;
  }
  const int ducked = static_cast<int>(
      std::lround(full * static_cast<double>(audio::Mixer::DUCK_GAIN)));
  CHECK(near(last, ducked));
  // About the attack time: 0.08 s is 3.4 blocks.
  CHECK(blocks <= 5);
  CHECK(near(block().back(), ducked)); // and it stays there

  m.stop(*phone);
  blocks = 0;
  while (last < full && blocks < 60) {
    const auto o = block();
    CHECK(o.front() >= last);
    last = o.back();
    ++blocks;
  }
  CHECK(last == full);
  CHECK(blocks >= 20); // the release is slower: 0.6 s is 26 blocks
  CHECK(blocks <= 30);

  m.ducking(false);
  m.play(*phone, true);
  for (int i = 0; i < 10; ++i) {
    CHECK(block().back() == full);
  }
}
