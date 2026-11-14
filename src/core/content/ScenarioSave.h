/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "core/content/Roster.h"
#include "core/content/Scenario.h"

namespace core::content {

// The save format version that carries a scenario block. Classic saves stay
// at version 3 and never have one (location-contract.md section 6.1).
inline constexpr int expanded_save_version = 4;

// The "scenario" block of a version 4 save (location-contract.md section
// 6.2): the scenario document, and the roster with every non-Classic
// definition embedded with its hash. Classic entries are referenced only.
[[nodiscard]] nlohmann::json
save_scenario_block(const Roster& roster,
                    const std::shared_ptr<const Scenario>& scenario);

struct SavedScenario {
  Roster roster;
  std::shared_ptr<const Scenario> scenario;
};

// Reads a scenario block back. Every embedded definition is read, validated
// and checked against its recorded hash before anything is returned; on any
// problem it returns nullopt and error names it ("save damaged" cases of
// section 6.6). Classic source hashes are not checked here.
[[nodiscard]] std::optional<SavedScenario>
read_scenario_block(const nlohmann::json& block, std::string& error);

} // namespace core::content
