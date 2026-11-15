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
#include <cstdio>
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

TEST_CASE("pack events are silent without a pack, and the original output "
          "stays bit-identical",
          "[audio][sfx][realism]") {
  // Two mixers with the same originals; one is also asked for every pack
  // event slot, which holds no original.
  audio::Mixer plain;
  audio::Mixer asked;
  for (audio::Mixer* m : {&plain, &asked}) {
    REQUIRE(m->add_effect(std::vector<std::int16_t>(3000, 1234)));
    REQUIRE(m->add_effect(std::vector<std::int16_t>(5000, -777), 1000));
    m->play(0, true);
    m->play(1, false);
  }
  for (std::size_t k = 0; k < audio::Mixer::PACK_EVENTS; ++k) {
    CHECK(asked.valid(audio::Mixer::EVENT_BASE + k));
    asked.play(audio::Mixer::EVENT_BASE + k, k % 2 == 0);
  }
  CHECK_FALSE(asked.valid(2)); // no third original
  const std::vector<std::int16_t> music(audio::MIX_BLOCK_SAMPLES, 3000);
  std::vector<std::int16_t> a(audio::MIX_BLOCK_SAMPLES);
  std::vector<std::int16_t> b(audio::MIX_BLOCK_SAMPLES);
  for (int i = 0; i < 12; ++i) {
    plain.render(a, music);
    asked.render(b, music);
    REQUIRE(a == b);
  }

  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  const std::int16_t bed = level_now(am);
  for (const char* id :
       {"guest-enter", "guest-leave", "payment", "plate-served", "kitchen-busy",
        "kitchen-idle", "door-close"}) {
    CHECK_FALSE(am.has_event(id));
    CHECK_FALSE(am.play_event(id));
    CHECK_FALSE(am.start_event(id));
    am.stop_event(id);
  }
  CHECK_FALSE(am.play_event("door-bell")); // no such event
  CHECK_FALSE(am.event_playing(audio::PackEvent::KITCHEN_BUSY));
  CHECK(level_now(am) == bed);
  CHECK(audio::pack_event_id(audio::PackEvent::PLATE_SERVED) == "plate-served");
  CHECK(audio::pack_event_slot(audio::PackEvent::GUEST_ENTER) ==
        audio::Mixer::EVENT_BASE);
  CHECK(audio::slot_event_id(audio::Mixer::EVENT_BASE + 2) == "door-close");
  CHECK(audio::slot_event_id(audio::Mixer::EVENT_BASE + 6) == "kitchen-idle");
  CHECK(audio::Mixer::EVENT_BASE == 57);
}

TEST_CASE("a pack sounds its events: one-shots once, the kitchen beds loop "
          "until stopped, with ambience on them",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const fs::path door = level_wav(g.pack / "door.wav", 0.1, 0.3);
  const fs::path kitchen = level_wav(g.pack / "kitchen.wav", 0.2, 0.1);
  const fs::path pot = level_wav(g.pack / "pot.wav", 0.1, 0.2);
  const json effects = json::array({{{"event", "guest-enter"},
                                     {"file", "door.wav"},
                                     {"sha256", sha_of(door)}},
                                    {{"event", "kitchen-busy"},
                                     {"file", "kitchen.wav"},
                                     {"sha256", sha_of(kitchen)}}});
  const json ambience = json::array(
      {{{"with", "kitchen-busy"},
        {"layers",
         json::array({{{"name", "pots"},
                       {"variants", json::array({sound(g.pack, pot)})},
                       {"interval_ms", json::array({500, 500})}}})}}});

  SECTION("version 1 has no pack events") {
    write_manifest(g.pack, 1, effects);
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    REQUIRE(done);
    const auto* o = outcome_of(done, "guest-enter");
    REQUIRE(o);
    CHECK_FALSE(o->replaced);
    CHECK(o->reason == "no such event");
    CHECK_FALSE(am.has_event("guest-enter"));
  }
  SECTION("version 2") {
    write_manifest(g.pack, 2, effects, ambience);
    const auto done = am.select_effects(EffectsSet::ENHANCED);
    REQUIRE(done);
    for (const char* id : {"guest-enter", "kitchen-busy"}) {
      const auto* o = outcome_of(done, id);
      REQUIRE(o);
      CHECK(o->replaced); // no source_sha256: nothing it replaces
      CHECK(o->slot >= static_cast<int>(audio::Mixer::EVENT_BASE));
    }
    const auto* layer = outcome_of(done, "kitchen-busy", "pots");
    REQUIRE(layer);
    CHECK(layer->replaced);
    CHECK(am.has_event("guest-enter"));
    CHECK_FALSE(am.has_event("payment")); // not in this pack: silent
    CHECK_FALSE(am.play_event("payment"));

    // A one-shot: heard once, then gone (0.1 s is under five blocks).
    REQUIRE(am.play_event("guest-enter", {.pan = 0.0f, .gain = 1.0f}));
    CHECK(level_now(am) == music_test::effect_level(stored(0.3)));
    for (int i = 0; i < 5; ++i) {
      music_test::fill(am);
    }
    CHECK(level_now(am) == 0);

    // The bed loops past its 0.2 s until it is stopped, and its scene runs.
    REQUIRE(am.start_event(audio::PackEvent::KITCHEN_BUSY));
    am.tick(0);
    for (int i = 0; i < 30; ++i) {
      music_test::fill(am);
    }
    CHECK(am.event_playing(audio::PackEvent::KITCHEN_BUSY));
    CHECK(level_now(am) == music_test::effect_level(stored(0.1)));
    am.tick(500);
    CHECK(near(level_now(am),
               music_test::effect_level(
                   static_cast<std::int16_t>(stored(0.1) + stored(0.2)))));
    am.stop_event("kitchen-busy");
    for (int i = 0; i < 6; ++i) {
      music_test::fill(am); // the pot ends too
    }
    CHECK_FALSE(am.event_playing(audio::PackEvent::KITCHEN_BUSY));
    CHECK(level_now(am) == 0);
  }
}

TEST_CASE("pool rules: a random gain in its range, a spacing, a voice limit "
          "per pool, and a dip under alerts",
          "[audio][sfx][realism]") {
  Game g;
  if (!g.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = g.a.am;
  const fs::path plate = level_wav(g.pack / "plate.wav", 0.5, 0.25);
  const fs::path door = level_wav(g.pack / "door.wav", 0.5, 0.2);
  const fs::path murmur = level_wav(g.pack / "murmur.wav", 4.0, 0.2);
  const json effects = json::array({{{"event", "plate-served"},
                                     {"file", "plate.wav"},
                                     {"sha256", sha_of(plate)},
                                     {"gain_jitter_db", 1.5}},
                                    {{"event", "guest-leave"},
                                     {"file", "door.wav"},
                                     {"sha256", sha_of(door)},
                                     {"min_spacing_ms", 1500},
                                     {"duck_db", 6}}});
  const json ambience = json::array(
      {{{"with", "restaurant-noise-1"},
        {"layers",
         json::array({{{"name", "murmur"},
                       {"variants", json::array({sound(g.pack, murmur)})},
                       {"interval_ms", json::array({250, 250})},
                       {"max_voices", 1},
                       {"duck_db", 6}},
                      {{"name", "bad"},
                       {"variants", json::array({sound(g.pack, murmur)})},
                       {"interval_ms", json::array({250, 250})},
                       {"gain_jitter_db", 9}}})}}});
  write_manifest(g.pack, 2, effects, ambience);
  const auto done = am.select_effects(EffectsSet::ENHANCED);
  REQUIRE(done);
  const auto* bad = outcome_of(done, "restaurant-noise-1", "bad");
  REQUIRE(bad);
  CHECK_FALSE(bad->replaced);
  CHECK(bad->reason.find("gain_jitter_db") != std::string::npos);

  SECTION("random gain") {
    const double nominal = stored(0.25);
    std::set<std::int16_t> levels;
    for (int k = 0; k < 30; ++k) {
      REQUIRE(am.play_event("plate-served"));
      const std::int16_t heard = level_now(am);
      levels.insert(heard);
      const double db =
          20.0 *
          std::log10(heard / static_cast<double>(music_test::effect_level(
                                 static_cast<std::int16_t>(nominal))));
      CHECK(std::abs(db) <= 1.5 + 0.01);
      for (int i = 0; i < 25; ++i) {
        music_test::fill(am); // 0.5 s, so it ends before the next
      }
    }
    CHECK(levels.size() > 10); // it does vary
  }
  SECTION("spacing") {
    REQUIRE(am.play_event("guest-leave"));
    const std::uint64_t skipped = am.mixer().spaced_out();
    music_test::fill(am);
    am.tick(1000);
    CHECK(am.play_event("guest-leave")); // taken, and skipped: too soon
    CHECK(am.mixer().spaced_out() == skipped + 1);
    am.tick(500);
    CHECK(am.play_event("guest-leave"));
    CHECK(am.mixer().spaced_out() == skipped + 1);
  }
  SECTION("a voice limit per pool, and the dip under an alert") {
    am.play_sound(SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
    am.tick(0);
    am.tick(250);
    const std::int16_t with_murmur = level_now(am);
    REQUIRE(am.mixer().extra_voices() == 1);
    const std::uint64_t dropped = am.mixer().dropped_voices();
    am.tick(250); // the murmur's second has no voice of its pool
    music_test::fill(am);
    CHECK(am.mixer().extra_voices() == 1);
    CHECK(am.mixer().dropped_voices() == dropped + 1);
    CHECK(near(with_murmur, music_test::effect_level(static_cast<std::int16_t>(
                                g.original(0) + stored(0.2)))));

    // The phone: the murmur dips 6 dB with the music's timing; the bed and
    // the phone itself do not.
    am.play_sound(SoundId::PHONE_RINGING, audio::LoopMode::Loop);
    for (int i = 0; i < 5; ++i) {
      music_test::fill(am);
    }
    CHECK(am.mixer().pool_duck() == 1.0f);
    const double dip = std::pow(10.0, -6.0 / 20.0);
    CHECK(near(level_now(am),
               music_test::effect_level(static_cast<std::int16_t>(std::lround(
                   g.original(0) + g.original(15) + stored(0.2) * dip)))));
    am.stop_sound(SoundId::PHONE_RINGING);
    for (int i = 0; i < 30; ++i) {
      music_test::fill(am);
    }
    CHECK(am.mixer().pool_duck() == 0.0f);
    CHECK(near(level_now(am), with_murmur));
  }
}

// Not run by default: it needs the original game and a private pack, neither
// of which is in the repo.
//
//   PLEGACY_TEST_GAME_PATH=<folder with PT.EXE> PLEGACY_SFX_PACK=<pack folder>
//     test_plegacy "[sfx-pack-v1]"
//
// Loads the originals and the pack as the game does, then plays each
// ambience scene's bed for 30 s of game time: its one-shots must come, and
// the mix must not clip. Prints one line per entry and per scene.
TEST_CASE("a private pack with ambience, through the real engine",
          "[.][sfx-pack-v1][audio]") {
  const auto game = test_support::game_path();
  const char* pack_env = std::getenv("PLEGACY_SFX_PACK");
  if (!game || !pack_env || !*pack_env) {
    SKIP("needs PLEGACY_TEST_GAME_PATH and PLEGACY_SFX_PACK");
  }
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  AudioManager& am = a.am;
  for (int m = 0; m <= 11; ++m) {
    am.load_m_container((*game / "SOUND" / ("M" + std::to_string(m))).string());
  }
  am.effects_loaded();
  am.effects_seed(7);
  am.effects_pack(fs::path(pack_env));
  const auto done = am.select_effects(EffectsSet::ENHANCED);
  INFO(done.error);
  REQUIRE(done);
  int refused = 0;
  std::size_t layers = 0;
  for (const audio::EffectOutcome& o : done.outcomes) {
    const std::string what =
        o.layer.empty() ? o.event : o.event + " / " + o.layer;
    std::printf("[sfx-pack-v1] %-36s %zu variant(s)  %s\n", what.c_str(),
                o.variants,
                !o.replaced        ? ("REFUSED: " + o.reason).c_str()
                : o.reason.empty() ? "ok"
                                   : ("partly: " + o.reason).c_str());
    refused += o.replaced && o.reason.empty() ? 0 : 1;
    layers += o.replaced && !o.layer.empty() ? 1 : 0;
  }
  CHECK(refused == 0);
  REQUIRE(layers > 0);

  const auto* bank = am.mixer().effect_bank();
  REQUIRE(bank);
  std::vector<std::size_t> beds;
  for (const auto& scene : bank->scenes) {
    beds.push_back(scene.bed);
  }
  constexpr int frames_per_block = static_cast<int>(audio::MIX_BLOCK_FRAMES);
  // A bed is an original event or a pack event (the kitchen's).
  const auto pack_event = [](std::size_t slot) {
    return static_cast<audio::PackEvent>(slot - audio::Mixer::EVENT_BASE);
  };
  for (const std::size_t bed : beds) {
    if (bed >= audio::Mixer::EVENT_BASE) {
      REQUIRE(am.start_event(pack_event(bed)));
    }
    else {
      am.play_sound(static_cast<SoundId>(bed), audio::LoopMode::Loop);
    }
    am.tick(0);
    int arrivals = 0;
    int peak = 0;
    std::size_t before = 0;
    std::uint64_t at_frames = 0;
    std::uint64_t ticked_ms = 0;
    while (at_frames < 30ull * 44100) {
      const auto out = music_test::fill(am);
      at_frames += frames_per_block;
      const std::uint64_t now_ms = at_frames * 1000 / 44100;
      am.tick(static_cast<std::uint32_t>(now_ms - ticked_ms));
      ticked_ms = now_ms;
      for (const std::int16_t s : out) {
        peak = std::max(peak, std::abs(static_cast<int>(s)));
      }
      const std::size_t voices = am.mixer().extra_voices();
      arrivals += voices > before ? static_cast<int>(voices - before) : 0;
      before = voices;
    }
    if (bed >= audio::Mixer::EVENT_BASE) {
      am.stop_event(pack_event(bed));
    }
    else {
      am.stop_sound(static_cast<SoundId>(bed));
    }
    for (int i = 0; i < 200 && am.mixer().extra_voices() > 0; ++i) {
      music_test::fill(am);
    }
    std::printf("[sfx-pack-v1] scene %-20s %d one-shots in 30 s, peak %d\n",
                std::string(audio::slot_event_id(bed)).c_str(), arrivals, peak);
    CHECK(arrivals > 0);
    CHECK(peak < 32767); // the mix never reaches full scale
  }

  // The pack's one-shot events, each heard on its own.
  for (const audio::PackEvent e :
       {audio::PackEvent::GUEST_ENTER, audio::PackEvent::GUEST_LEAVE,
        audio::PackEvent::DOOR_CLOSE, audio::PackEvent::PAYMENT,
        audio::PackEvent::PLATE_SERVED}) {
    const std::string id(audio::pack_event_id(e));
    if (!am.has_event(id)) {
      std::printf("[sfx-pack-v1] event %-20s not in this pack\n", id.c_str());
      continue;
    }
    am.tick(5000); // past any spacing
    REQUIRE(am.play_event(e));
    int peak = 0;
    for (int i = 0; i < 40; ++i) {
      for (const std::int16_t s : music_test::fill(am)) {
        peak = std::max(peak, std::abs(static_cast<int>(s)));
      }
    }
    std::printf("[sfx-pack-v1] event %-20s peak %d\n", id.c_str(), peak);
    CHECK(peak > 0);
    CHECK(peak < 32767);
  }
}
