/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/content/ContentKey.h"
#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"

class GameContext;
class GameState;
struct GameRules;
namespace init {
struct Config;
}

namespace core::content {

// The Expanded scenarios shipped in the data folder: data/scenarios/<file>.json
// and the locations of their rosters under data/locations.
struct ShippedScenario {
  std::string_view file;
  // What the new game menu calls it.
  std::string_view menu_label;
};

// The North Pole destination and its presets (NORTH-POLE-FLAGSHIP.md): all
// four play the same pinned location; they differ in start, goals and rules,
// and leave the game's five difficulty levels as they are.
inline constexpr std::string_view north_pole_menu_label =
    "Santa's Pizza Village";

inline constexpr ShippedScenario cozy_village{
    .file = "north-pole-cozy-village",
    .menu_label = "Cozy Village",
};
inline constexpr ShippedScenario workshop_rush{
    .file = "north-pole-workshop-rush",
    .menu_label = "Workshop Rush",
};
inline constexpr ShippedScenario pizza_empire{
    .file = "north-pole-pizza-empire",
    .menu_label = "Santa's Pizza Empire",
};
inline constexpr ShippedScenario village_sandbox{
    .file = "north-pole-sandbox",
    .menu_label = "Sandbox",
};

inline constexpr std::array<ShippedScenario, 4> north_pole_presets = {
    cozy_village, workshop_rush, pizza_empire, village_sandbox};

// data/ as the game finds it (platform_paths::data_dir()).
[[nodiscard]] std::filesystem::path shipped_data_dir();

// The valid definition of key among data/locations/**/*.json, or null with
// the findings that ruled the candidates out.
[[nodiscard]] std::shared_ptr<const LocationDefinition> find_shipped_location(
    const ContentKey& key, Findings& findings,
    const std::filesystem::path& data_dir = shipped_data_dir());

// Reads data/scenarios/<file>.json and its locations and sets up config for
// it with configure_new_game (base_rules are the player's options). False,
// with findings, when the scenario or one of its locations is missing or
// invalid. The players are left to the caller.
bool prepare_shipped_scenario(
    std::string_view file, init::Config& config, const GameRules& base_rules,
    Findings& findings,
    const std::filesystem::path& data_dir = shipped_data_dir());

// A whole new game of a shipped scenario as scripted runs start one: seeded
// if a seed is given, player 1 the only person, the original characters
// 0..7 and random company names, as the tutorial does.
bool start_shipped_scenario(GameState& state, GameContext& context,
                            std::string_view file,
                            std::optional<std::uint32_t> seed,
                            Findings& findings);

} // namespace core::content
