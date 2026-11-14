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
constexpr int manifest_version = 1;
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

// The entry's sound, or the reason it is refused.
std::unique_ptr<Mixer::EffectBank::Sound>
load_sound(const json& entry, const fs::path& file, const EffectLimits& limits,
           std::uint64_t& pack_bytes, std::string& why) {
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
  if (core::content::Sha256::hex(*bytes) != text(entry, "sha256")) {
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
  const std::size_t head = frames_of(number(entry, "trim_start_ms", 0));
  const std::size_t tail = frames_of(number(entry, "trim_end_ms", 0));
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
  const std::size_t loop_start = frames_of(number(entry, "loop_start_ms", 0));
  if (loop_start >= sound->samples.size() / 2) {
    why = "its loop starts after its end";
    return nullptr;
  }
  sound->start_offset = loop_start * 2;
  const double gain_db = std::clamp(number(entry, "gain_db", 0.0), -60.0, 12.0);
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

} // namespace

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
      *version != manifest_version) {
    return refuse("its manifest.json has a version this game does not read");
  }
  const auto effects = doc->find("effects");
  if (effects == doc->end() || !effects->is_array()) {
    return refuse("its manifest.json lists no effects");
  }
  if (effects->size() > limits.max_entries) {
    return refuse("its manifest.json lists more than " +
                  std::to_string(limits.max_entries) + " effects");
  }

  EffectPack pack;
  if (const auto info = doc->find("pack");
      info != doc->end() && info->is_object()) {
    pack.id = music::clean_text(text(*info, "id"), 128);
    pack.revision = static_cast<std::uint32_t>(
        std::clamp(number(*info, "revision", 0), 0.0, 4.0e9));
  }
  pack.bank = std::make_unique<Mixer::EffectBank>();
  std::set<SoundId> seen;
  std::uint64_t pack_bytes = 0;
  for (const json& entry : *effects) {
    EffectOutcome outcome;
    if (!entry.is_object()) {
      outcome.reason = "not an entry";
      pack.outcomes.push_back(std::move(outcome));
      continue;
    }
    outcome.event = music::clean_text(text(entry, "event"), 64);
    const auto id = effect_from_event(outcome.event);
    std::string reason;
    if (!id) {
      reason = "no such event";
    }
    else {
      outcome.slot = static_cast<int>(*id);
      const auto slot = entry.find("slot");
      const std::size_t i = static_cast<std::size_t>(*id);
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
      else if (!is_hex64(text(entry, "sha256"))) {
        reason = "it has no sha256";
      }
      else if (!is_hex64(text(entry, "source_sha256"))) {
        reason = "it has no source_sha256";
      }
      else if (i >= original_sha256.size() || original_sha256[i].empty()) {
        reason = "the original it replaces is not loaded";
      }
      else if (original_sha256[i] != text(entry, "source_sha256")) {
        reason = "it was made for a different original";
      }
      else if (const auto file =
                   contained(folder, text(entry, "file"), reason)) {
        if (auto sound = load_sound(entry, *file, limits, pack_bytes, reason)) {
          pack.bank->slots[i] = std::move(sound);
          outcome.replaced = true;
        }
      }
    }
    outcome.reason = outcome.replaced ? std::string{} : reason;
    pack.outcomes.push_back(std::move(outcome));
  }
  return pack;
}

} // namespace audio
