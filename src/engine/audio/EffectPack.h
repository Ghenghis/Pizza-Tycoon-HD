/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/AudioManager.h"

// Sound effect replacement by event (docs/studio/notes/sfx-overrides.md).
namespace audio {

// The stable event id of each effect slot ("phone-ringing"). Slots keep
// their numbers; a pack names events, never positions.
[[nodiscard]] std::string_view effect_event_id(SoundId id);
[[nodiscard]] std::optional<SoundId> effect_from_event(std::string_view event);
// What wins when too much plays at once, as the 1994 effects are used: the
// phone ringing and the police are alerts; the restaurant and the traffic
// are ambience; the rest answer the player.
[[nodiscard]] Priority default_priority(SoundId id);
[[nodiscard]] std::string_view priority_name(Priority priority);
[[nodiscard]] std::optional<Priority> priority_from_name(std::string_view name);

struct EffectLimits {
  std::uint64_t max_manifest_bytes = std::uint64_t{1} << 20;
  std::size_t max_entries = 256;
  std::uint64_t max_file_bytes = std::uint64_t{16} << 20; // one file
  std::uint32_t max_seconds = 120;                        // one sound
  std::uint64_t max_pack_bytes = std::uint64_t{64} << 20; // decoded, all
  std::size_t max_scenes = 8;                             // ambience scenes
  std::size_t max_layers = 8;                             // per scene
  std::uint32_t min_interval_ms = 250; // between one layer's one-shots
  std::uint32_t max_interval_ms = 600000;
};

struct EffectPack {
  std::string id;
  std::uint32_t revision = 0;
  std::unique_ptr<Mixer::EffectBank> bank;
  // One per manifest entry, then one per ambience layer.
  std::vector<EffectOutcome> outcomes;
};

// Reads folder/manifest.json (version 1, or 2 with variants, priorities
// and ambience), then checks and decodes every replacement it names: its path
// stays inside folder, its bytes match their SHA-256, the original it replaces
// matches source_sha256 (original_sha256 holds the loaded originals by slot),
// and its size, length and format are within limits. A refused entry leaves its
// slot to the original. Returns nullopt, with the reason, when the manifest
// itself cannot be used.
[[nodiscard]] std::optional<EffectPack>
load_effect_pack(const std::filesystem::path& folder,
                 std::span<const std::string> original_sha256,
                 const EffectLimits& limits, std::string* error);

} // namespace audio
