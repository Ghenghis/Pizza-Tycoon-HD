/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/audio/EffectPack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <system_error>

#include "core/content/Sha256.h"
#include "engine/music/JsonStore.h"
#include "engine/music/TrackDecoder.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace audio {

namespace {

constexpr std::string_view manifest_format = "pthd-sfx-overrides";
constexpr int oldest_version = 1;
constexpr int manifest_version = 2; // variants, priorities, ambience
constexpr int rate = 44100;

constexpr std::array<std::string_view, static_cast<std::size_t>(SoundId::COUNT)>
    event_ids = {
        "restaurant-noise-1", "restaurant-noise-2", "restaurant-noise-3",
        "traffic-1",          "traffic-2",          "dial-tone-0",
        "dial-tone-1",        "dial-tone-2",        "dial-tone-3",
        "dial-tone-4",        "dial-tone-5",        "dial-tone-6",
        "dial-tone-7",        "dial-tone-8",        "dial-tone-9",
        "phone-ringing",      "phone-pickup",       "chopper",
        "weapon-1",           "weapon-2",           "weapon-3",
        "weapon-4",           "police-siren",
};

bool is_hex64(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

constexpr std::array<std::string_view,
                     static_cast<std::size_t>(PackEvent::COUNT)>
    pack_event_ids = {"guest-enter",  "guest-leave",  "door-close",  "payment",
                      "plate-served", "kitchen-busy", "kitchen-idle"};
static_assert(pack_event_ids.size() == Mixer::PACK_EVENTS);

constexpr std::array<std::string_view, 3> priority_names = {
    "ambience", "feedback", "alert"};

std::string text(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

double number(const json& j, const char* key, double fallback) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number() && std::isfinite(it->get<double>())
             ? it->get<double>()
             : fallback;
}

// The file the entry names, when it is a plain relative path that stays
// inside folder, links resolved.
std::optional<fs::path> contained(const fs::path& folder,
                                  const std::string& name, std::string& why) {
  const fs::path rel = music::path_from_utf8(name);
  if (name.empty() || rel.is_absolute() || rel.has_root_name() ||
      rel.has_root_directory()) {
    why = "its file must be a path inside the pack";
    return std::nullopt;
  }
  const fs::path normal = rel.lexically_normal();
  if (normal.empty() || *normal.begin() == "..") {
    why = "its file is outside the pack";
    return std::nullopt;
  }
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(folder, ec);
  const fs::path full = fs::weakly_canonical(folder / normal, ec);
  if (ec) {
    why = "its file cannot be found";
    return std::nullopt;
  }
  const fs::path inside = full.lexically_relative(base);
  if (inside.empty() || *inside.begin() == "..") {
    why = "its file is outside the pack";
    return std::nullopt;
  }
  return full;
}

// The slot an event plays in: an original's, or (version 2) a pack event's.
std::optional<std::size_t> slot_of(std::string_view event, bool v2) {
  if (const auto id = effect_from_event(event)) {
    return static_cast<std::size_t>(*id);
  }
  if (const auto e = pack_event_from_name(event); e && v2) {
    return pack_event_slot(*e);
  }
  return std::nullopt;
}

// A number field within [lo, hi], absent meaning fallback; nullopt when it
// is there but not such a number.
std::optional<double> bounded(const json& j, const char* key, double lo,
                              double hi, double fallback) {
  const auto it = j.find(key);
  if (it == j.end()) {
    return fallback;
  }
  if (!it->is_number() || !std::isfinite(it->get<double>()) ||
      it->get<double>() < lo || it->get<double>() > hi) {
    return std::nullopt;
  }
  return it->get<double>();
}

// The pool rules of an entry or a layer (version 2), or why they are wrong.
std::optional<Mixer::EffectBank::Rules> rules_of(const json& j,
                                                 std::string& why) {
  const auto jitter = bounded(j, "gain_jitter_db", 0.0, 6.0, 0.0);
  const auto spacing = bounded(j, "min_spacing_ms", 0.0, 60000.0, 0.0);
  const auto duck = bounded(j, "duck_db", 0.0, 24.0, 0.0);
  if (!jitter) {
    why = "its gain_jitter_db must be 0 to 6";
    return std::nullopt;
  }
  if (!spacing) {
    why = "its min_spacing_ms must be 0 to 60000";
    return std::nullopt;
  }
  if (!duck) {
    why = "its duck_db must be 0 to 24";
    return std::nullopt;
  }
  return Mixer::EffectBank::Rules{
      .jitter_db = static_cast<float>(*jitter),
      .spacing_ms = static_cast<std::uint32_t>(*spacing),
      .duck_gain = static_cast<float>(std::pow(10.0, -*duck / 20.0))};
}

// A sound's own field, else the entry's (the defaults of its variants).
double field(const json& spec, const json& entry, const char* key,
             double fallback) {
  return number(spec, key, number(entry, key, fallback));
}

// The sound spec names (an entry, or one of its variants, with the entry's
// fields as defaults), or the reason it is refused.
std::unique_ptr<Mixer::EffectBank::Sound>
load_sound(const json& spec, const json& entry, const fs::path& file,
           const EffectLimits& limits, std::uint64_t& pack_bytes,
           std::string& why) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec) {
    why = "its file is missing";
    return nullptr;
  }
  if (size > limits.max_file_bytes) {
    why = "its file is larger than " + music::mib(limits.max_file_bytes);
    return nullptr;
  }
  std::string read_error;
  const auto bytes =
      music::read_text_file(file, limits.max_file_bytes, &read_error);
  if (!bytes) {
    why = "its file cannot be read";
    return nullptr;
  }
  if (core::content::Sha256::hex(*bytes) != text(spec, "sha256")) {
    why = "its file does not match its sha256";
    return nullptr;
  }

  music::Limits decode;
  decode.max_file_bytes = limits.max_file_bytes;
  decode.max_seconds = limits.max_seconds;
  decode.max_header_bytes =
      std::min<std::uint64_t>(decode.max_header_bytes, limits.max_file_bytes);
  auto decoder = music::open_builtin_decoder(file, decode, &why);
  if (!decoder) {
    return nullptr;
  }
  music::TrackReader reader;
  if (!reader.open(std::move(decoder), &why)) {
    return nullptr;
  }
  const std::size_t max_frames =
      static_cast<std::size_t>(limits.max_seconds) * rate;
  auto sound = std::make_unique<Mixer::EffectBank::Sound>();
  std::vector<int16_t> chunk(4096 * 2);
  while (sound->samples.size() / 2 < max_frames) {
    const std::size_t got = reader.read(chunk);
    if (got == 0) {
      break;
    }
    sound->samples.insert(sound->samples.end(), chunk.begin(),
                          chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
  }
  if (reader.damaged() || !reader.at_end()) {
    why = "its file is damaged or too long";
    return nullptr;
  }

  // Trims, the loop start and the gain: per sound, never inherited from the
  // original (the chopper's 500-sample skip belongs to the 1994 sound).
  const auto frames_of = [](double ms) {
    return static_cast<std::size_t>(std::max(ms, 0.0) * rate / 1000.0);
  };
  const std::size_t total = sound->samples.size() / 2;
  const std::size_t head = frames_of(field(spec, entry, "trim_start_ms", 0));
  const std::size_t tail = frames_of(field(spec, entry, "trim_end_ms", 0));
  if (head + tail >= total) {
    why = "its trims leave no sound";
    return nullptr;
  }
  sound->samples.erase(sound->samples.end() -
                           static_cast<std::ptrdiff_t>(tail * 2),
                       sound->samples.end());
  sound->samples.erase(sound->samples.begin(),
                       sound->samples.begin() +
                           static_cast<std::ptrdiff_t>(head * 2));
  const std::size_t loop_start =
      frames_of(field(spec, entry, "loop_start_ms", 0));
  if (loop_start >= sound->samples.size() / 2) {
    why = "its loop starts after its end";
    return nullptr;
  }
  sound->start_offset = loop_start * 2;
  const double gain_db =
      std::clamp(field(spec, entry, "gain_db", 0.0), -60.0, 12.0);
  if (gain_db != 0.0) {
    const double g = std::pow(10.0, gain_db / 20.0);
    for (int16_t& s : sound->samples) {
      s = static_cast<int16_t>(
          std::lrint(std::clamp(s * g, -32768.0, 32767.0)));
    }
  }
  const std::uint64_t used = sound->samples.size() * sizeof(int16_t);
  if (pack_bytes + used > limits.max_pack_bytes) {
    why = "the pack is over its memory budget of " +
          music::mib(limits.max_pack_bytes);
    return nullptr;
  }
  pack_bytes += used;
  return sound;
}

// The sounds an entry or a layer names: its variants (version 2), or the
// entry itself. Loads every one; returns those that load, with the reasons
// of the others in refused (by variant number, from 1) or, for a single
// sound, in why.
std::vector<Mixer::EffectBank::Sound>
load_variants(const json& entry, bool has_variants, const fs::path& folder,
              const EffectLimits& limits, std::uint64_t& pack_bytes,
              std::string& why, std::string& refused) {
  std::vector<Mixer::EffectBank::Sound> sounds;
  const auto one = [&](const json& spec, std::string& reason) {
    if (!is_hex64(text(spec, "sha256"))) {
      reason = "it has no sha256";
      return;
    }
    if (const auto file = contained(folder, text(spec, "file"), reason)) {
      if (auto sound =
              load_sound(spec, entry, *file, limits, pack_bytes, reason)) {
        sounds.push_back(std::move(*sound));
      }
    }
  };
  if (!has_variants) {
    one(entry, why);
    return sounds;
  }
  const json& list = entry["variants"];
  for (std::size_t i = 0; i < list.size(); ++i) {
    std::string reason;
    if (!list[i].is_object()) {
      reason = "not a variant";
    }
    else {
      const std::size_t before = sounds.size();
      one(list[i], reason);
      if (sounds.size() > before) {
        continue;
      }
    }
    refused += (refused.empty() ? "" : "; ") + std::string("variant ") +
               std::to_string(i + 1) + ": " + reason;
  }
  if (sounds.empty()) {
    why = refused;
  }
  return sounds;
}

// Why an entry's variants list cannot be used, if so.
std::string bad_variants(const json& entry) {
  const json& list = entry["variants"];
  if (!list.is_array() || list.empty()) {
    return "its variants must be a list of sounds";
  }
  if (list.size() > Mixer::MAX_VARIANTS) {
    return "it has more than " + std::to_string(Mixer::MAX_VARIANTS) +
           " variants";
  }
  return {};
}

// [lo, hi] from a two-number list, if it is one.
std::optional<std::pair<double, double>> range(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 2 ||
      !(*it)[0].is_number() || !(*it)[1].is_number()) {
    return std::nullopt;
  }
  const double lo = (*it)[0].get<double>();
  const double hi = (*it)[1].get<double>();
  if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) {
    return std::nullopt;
  }
  return std::pair{lo, hi};
}

} // namespace

Priority default_priority(SoundId id) {
  switch (id) {
  case SoundId::PHONE_RINGING:
  case SoundId::POLICE_SIREN:
    return Priority::ALERT;
  case SoundId::RESTAURANT_NOISE_1:
  case SoundId::RESTAURANT_NOISE_2:
  case SoundId::RESTAURANT_NOISE_3:
  case SoundId::TRAFFIC_1:
  case SoundId::TRAFFIC_2:
    return Priority::AMBIENCE;
  default:
    return Priority::FEEDBACK;
  }
}

Priority default_priority(PackEvent event) {
  return event == PackEvent::KITCHEN_BUSY || event == PackEvent::KITCHEN_IDLE
             ? Priority::AMBIENCE
             : Priority::FEEDBACK;
}

std::string_view pack_event_id(PackEvent event) {
  const auto i = static_cast<std::size_t>(event);
  return i < pack_event_ids.size() ? pack_event_ids[i] : std::string_view{};
}

std::optional<PackEvent> pack_event_from_name(std::string_view event) {
  for (std::size_t i = 0; i < pack_event_ids.size(); ++i) {
    if (pack_event_ids[i] == event) {
      return static_cast<PackEvent>(i);
    }
  }
  return std::nullopt;
}

std::size_t pack_event_slot(PackEvent event) {
  return Mixer::EVENT_BASE + static_cast<std::size_t>(event);
}

std::string_view slot_event_id(std::size_t slot) {
  if (slot >= Mixer::EVENT_BASE && slot < Mixer::MAX_EFFECTS) {
    return pack_event_id(static_cast<PackEvent>(slot - Mixer::EVENT_BASE));
  }
  return effect_event_id(static_cast<SoundId>(slot));
}

std::string_view priority_name(Priority priority) {
  const auto i = static_cast<std::size_t>(priority);
  return i < priority_names.size() ? priority_names[i] : std::string_view{};
}

std::optional<Priority> priority_from_name(std::string_view name) {
  for (std::size_t i = 0; i < priority_names.size(); ++i) {
    if (priority_names[i] == name) {
      return static_cast<Priority>(i);
    }
  }
  return std::nullopt;
}

std::string to_string(EffectsSet set) {
  return set == EffectsSet::ENHANCED ? "Enhanced" : "Original";
}

std::string_view effect_event_id(SoundId id) {
  const auto i = static_cast<std::size_t>(id);
  return i < event_ids.size() ? event_ids[i] : std::string_view{};
}

std::optional<SoundId> effect_from_event(std::string_view event) {
  for (std::size_t i = 0; i < event_ids.size(); ++i) {
    if (event_ids[i] == event) {
      return static_cast<SoundId>(i);
    }
  }
  return std::nullopt;
}

std::optional<EffectPack>
load_effect_pack(const fs::path& folder,
                 std::span<const std::string> original_sha256,
                 const EffectLimits& limits, std::string* error) {
  const auto refuse = [error](std::string why) -> std::optional<EffectPack> {
    if (error) {
      *error = std::move(why);
    }
    return std::nullopt;
  };
  std::string why;
  const fs::path manifest = folder / "manifest.json";
  const auto body =
      music::read_text_file(manifest, limits.max_manifest_bytes, &why);
  if (!body) {
    return refuse(why.empty() ? "the pack has no manifest.json"
                              : "its manifest.json " + why);
  }
  const auto doc = music::parse_json(*body, 8, 100000, &why);
  if (!doc || !doc->is_object()) {
    return refuse("its manifest.json is damaged" +
                  (why.empty() ? std::string{} : " (" + why + ")"));
  }
  if (text(*doc, "format") != manifest_format) {
    return refuse("its manifest.json is not an effects pack");
  }
  const auto version = doc->find("version");
  if (version == doc->end() || !version->is_number_integer() ||
      *version < oldest_version || *version > manifest_version) {
    return refuse("its manifest.json has a version this game does not read");
  }
  // Variants, priorities and ambience are read from version 2 on; a
  // version 1 pack reads as it always did.
  const bool v2 = *version >= 2;
  const auto effects = doc->find("effects");
  if (effects == doc->end() || !effects->is_array()) {
    return refuse("its manifest.json lists no effects");
  }
  if (effects->size() > limits.max_entries) {
    return refuse("its manifest.json lists more than " +
                  std::to_string(limits.max_entries) + " effects");
  }
  const json no_scenes = json::array();
  const json* scenes = &no_scenes;
  if (const auto it = doc->find("ambience"); v2 && it != doc->end()) {
    if (!it->is_array()) {
      return refuse("its manifest.json has an ambience that is not a list");
    }
    if (it->size() > limits.max_scenes) {
      return refuse("its manifest.json lists more than " +
                    std::to_string(limits.max_scenes) + " ambience scenes");
    }
    for (const json& scene : *it) {
      const auto layers =
          scene.is_object() ? scene.find("layers") : scene.end();
      if (!scene.is_object() || layers == scene.end() || !layers->is_array()) {
        return refuse("its manifest.json has an ambience scene without layers");
      }
      if (layers->size() > limits.max_layers) {
        return refuse(
            "its manifest.json has an ambience scene with more than " +
            std::to_string(limits.max_layers) + " layers");
      }
    }
    scenes = &*it;
  }

  EffectPack pack;
  if (const auto info = doc->find("pack");
      info != doc->end() && info->is_object()) {
    pack.id = music::clean_text(text(*info, "id"), 128);
    pack.revision = static_cast<std::uint32_t>(
        std::clamp(number(*info, "revision", 0), 0.0, 4.0e9));
  }
  pack.bank = std::make_unique<Mixer::EffectBank>();
  std::set<std::size_t> seen;
  std::uint64_t pack_bytes = 0;
  for (const json& entry : *effects) {
    EffectOutcome outcome;
    if (!entry.is_object()) {
      outcome.reason = "not an entry";
      pack.outcomes.push_back(std::move(outcome));
      continue;
    }
    outcome.event = music::clean_text(text(entry, "event"), 64);
    const auto id = slot_of(outcome.event, v2);
    // A pack event has no original, so nothing to have been made for.
    const bool pack_only = id && *id >= Mixer::EVENT_BASE;
    std::string reason;
    std::string rules_why;
    const auto rules = v2 ? rules_of(entry, rules_why)
                          : std::optional{Mixer::EffectBank::Rules{}};
    if (!id) {
      reason = "no such event";
    }
    else {
      outcome.slot = static_cast<int>(*id);
      const auto slot = entry.find("slot");
      const std::size_t i = *id;
      if (slot != entry.end() &&
          (!slot->is_number_integer() || *slot != outcome.slot)) {
        reason = "its slot is not the event's (" +
                 std::to_string(outcome.slot) + ")";
      }
      else if (!seen.insert(*id).second) {
        reason = "the event is listed twice";
      }
      else if (const std::string fb = text(entry, "fallback");
               !fb.empty() && fb != "original") {
        reason = "its fallback must be \"original\"";
      }
      else if (const bool listed = v2 && entry.contains("variants");
               listed && !bad_variants(entry).empty()) {
        reason = bad_variants(entry);
      }
      else if (!listed && !is_hex64(text(entry, "sha256"))) {
        reason = "it has no sha256";
      }
      else if (const std::string p = text(entry, "priority");
               v2 && entry.contains("priority") && !priority_from_name(p)) {
        reason = "its priority must be alert, feedback or ambience";
      }
      else if (!rules) {
        reason = rules_why;
      }
      else if (!pack_only && !is_hex64(text(entry, "source_sha256"))) {
        reason = "it has no source_sha256";
      }
      else if (!pack_only &&
               (i >= original_sha256.size() || original_sha256[i].empty())) {
        reason = "the original it replaces is not loaded";
      }
      else if (!pack_only &&
               original_sha256[i] != text(entry, "source_sha256")) {
        reason = "it was made for a different original";
      }
      else {
        std::string refused;
        auto sounds =
            load_variants(entry, v2 && entry.contains("variants"), folder,
                          limits, pack_bytes, reason, refused);
        if (!sounds.empty()) {
          outcome.replaced = true;
          outcome.variants = sounds.size();
          auto pool = std::make_unique<Mixer::EffectBank::Pool>();
          pool->variants = std::move(sounds);
          pack.bank->slots[i] = std::move(pool);
          pack.bank->rules[i] = *rules;
          if (v2) {
            pack.bank->priorities[i] =
                priority_from_name(text(entry, "priority"));
          }
          reason = refused;
        }
      }
    }
    outcome.reason = reason;
    pack.outcomes.push_back(std::move(outcome));
  }

  // Ambience: one-shots over a bed, which is one of the events.
  for (const json& scene : *scenes) {
    const std::string bed = music::clean_text(text(scene, "with"), 64);
    const auto bed_id = slot_of(bed, true); // ambience is version 2
    Mixer::EffectBank::Scene made;
    made.bed = bed_id ? *bed_id : 0;
    for (const json& layer : scene["layers"]) {
      EffectOutcome outcome;
      outcome.event = bed;
      outcome.slot = bed_id ? static_cast<int>(*bed_id) : -1;
      std::string reason;
      std::string rules_why;
      const auto rules = layer.is_object()
                             ? rules_of(layer, rules_why)
                             : std::optional{Mixer::EffectBank::Rules{}};
      const auto voices =
          layer.is_object()
              ? bounded(layer, "max_voices", 0.0, Mixer::MAX_EXTRA_VOICES, 0.0)
              : std::optional{0.0};
      // Read into plain values: GCC's -O3 flow analysis reports the members
      // of a ternary-built optional<pair> as maybe uninitialized.
      const auto interval_read =
          layer.is_object() ? range(layer, "interval_ms")
                            : std::optional<std::pair<double, double>>{};
      const bool has_interval = interval_read.has_value();
      const double interval_least = has_interval ? interval_read->first : 0.0;
      const double interval_most = has_interval ? interval_read->second : 0.0;
      const auto pan = layer.is_object() && layer.contains("pan")
                           ? range(layer, "pan")
                           : std::optional{std::pair{0.0, 0.0}};
      if (!layer.is_object()) {
        reason = "not a layer";
      }
      else if ((outcome.layer = music::clean_text(text(layer, "name"), 64))
                   .empty()) {
        reason = "it has no name";
      }
      else if (!bed_id) {
        reason = "no such event";
      }
      else if (!layer.contains("variants") || !bad_variants(layer).empty()) {
        reason = layer.contains("variants") ? bad_variants(layer)
                                            : "it has no variants";
      }
      else if (!has_interval || interval_least < limits.min_interval_ms ||
               interval_most > limits.max_interval_ms) {
        reason = "its interval_ms must be [least, most], from " +
                 std::to_string(limits.min_interval_ms) + " to " +
                 std::to_string(limits.max_interval_ms);
      }
      else if (!pan || pan->first < -1.0 || pan->second > 1.0) {
        reason = "its pan must be [left, right], from -1 to 1";
      }
      else if (!rules) {
        reason = rules_why;
      }
      else if (!voices || *voices != std::floor(*voices)) {
        reason = "its max_voices must be a whole number, 0 to " +
                 std::to_string(Mixer::MAX_EXTRA_VOICES);
      }
      else {
        std::string refused;
        auto sounds = load_variants(layer, true, folder, limits, pack_bytes,
                                    reason, refused);
        if (!sounds.empty()) {
          outcome.replaced = true;
          outcome.variants = sounds.size();
          made.layers.push_back(Mixer::EffectBank::Layer{
              .name = outcome.layer,
              .variants = std::move(sounds),
              .min_interval_ms = static_cast<std::uint32_t>(interval_least),
              .max_interval_ms = static_cast<std::uint32_t>(interval_most),
              .min_pan = static_cast<float>(pan->first),
              .max_pan = static_cast<float>(pan->second),
              .rules = *rules,
              .max_voices = static_cast<std::size_t>(*voices)});
          reason = refused;
        }
      }
      outcome.reason = reason;
      pack.outcomes.push_back(std::move(outcome));
    }
    if (!made.layers.empty()) {
      pack.bank->scenes.push_back(std::move(made));
    }
  }
  return pack;
}

} // namespace audio
