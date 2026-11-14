/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/content/ContentKey.h"
#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"
#include "core/events/MilestoneId.h"
#include "core/food/PizzaJudging.h"

struct GameRules;
namespace init {
struct Config;
}

namespace core::content {

class Roster;

// A scenario as read from docs/studio/scenario.schema.json
// (location-contract.md section 11): where a game starts, its win rule and
// the objectives the goal observer watches.

enum class ScenarioStart {
  FreeMode,
  QuickStart,
};

enum class WinMode {
  // No market share ends the game; the goal observer reports the goal.
  None,
  MarketShare,
};

enum class WinScope {
  AnyLocation,
  AllLocations,
  Listed,
};

enum class AfterGoal {
  ContinueSandbox,
  EndGame,
};

enum class Polarity {
  // Met once the milestone is recorded for the player.
  Achieve,
  // Met for as long as it never is.
  Avoid,
};

enum class Pin {
  Classic,
  Exact,
  CurrentPublished,
};

struct Objective {
  std::string id;
  core::events::MilestoneId milestone{};
  Polarity polarity{Polarity::Achieve};
  std::string label;
  // Counted from the first day of the game.
  std::optional<int> within_game_days;
};

struct ScenarioLocation {
  ContentKey key = *ContentKey::parse("pthd:unnamed");
  Pin pin{Pin::CurrentPublished};
  // For Pin::Exact.
  int revision{0};
  std::string definition_sha256;
};

// The game_rules a scenario sets; unset members keep the player's options.
struct ScenarioRuleOverrides {
  std::optional<bool> fix_legacy_bugs;
  std::optional<bool> mafia_shakedowns_target_all_restaurants;
  std::optional<bool> demand_fulfillment_popularity;
  std::optional<PizzaJudging> pizza_judging;
  std::optional<bool> cook_book;
  std::optional<bool> police;
  std::optional<bool> suppliers_remember_missed_payments;
};

struct Scenario {
  ContentKey id = *ContentKey::parse("pthd:unnamed");
  int revision{1};
  std::string title;
  std::string subtitle;
  std::string summary;
  std::string intro;
  std::string goal_summary;
  std::string victory_summary;

  std::vector<ScenarioLocation> roster;
  ContentKey start_key = *ContentKey::parse("pthd:unnamed");
  ScenarioStart start{ScenarioStart::FreeMode};
  bool quickstart_big{false};
  int difficulty{1};

  WinMode win_mode{WinMode::None};
  WinScope win_scope{WinScope::AnyLocation};
  std::vector<ContentKey> win_locations;
  int win_percent{0};
  bool travel_disabled{true};
  ScenarioRuleOverrides game_rules;

  std::vector<Objective> goal;
  std::vector<Objective> optional_objectives;
  AfterGoal after_goal{AfterGoal::ContinueSandbox};

  // SHA-256 of the canonical form, and the canonical form itself, for the
  // save's scenario block.
  std::string sha256;
  std::string canonical_document;
};

struct ScenarioReadResult {
  std::shared_ptr<const Scenario> scenario;
  Findings findings;
  [[nodiscard]] bool ok() const {
    return scenario != nullptr && !has_errors(findings);
  }
};

// Reads the shape (SCHEMA-1) and then checks the scenario rules: SCN-1 a
// location appears once and is pinned "classic" exactly when it is an
// original city, SCN-2 the start is in the roster, SCN-3 continuing as a
// sandbox needs win mode none, SCN-4 city-scoped milestones need a single
// location, SCN-5 travel is disabled only with a single location. The
// scenario is set only when there is no error.
[[nodiscard]] ScenarioReadResult read_scenario(const nlohmann::json& document);
[[nodiscard]] ScenarioReadResult read_scenario_text(std::string_view text);
[[nodiscard]] ScenarioReadResult
read_scenario_file(const std::filesystem::path& path);

// The milestones scenario objectives may use (scenario.schema.json
// supported_milestone), by schema name.
[[nodiscard]] std::optional<core::events::MilestoneId>
supported_milestone(std::string_view name);
[[nodiscard]] std::string_view milestone_name(core::events::MilestoneId id);

using DefinitionLookup =
    std::function<std::shared_ptr<const LocationDefinition>(const ContentKey&)>;

// The game roster of a scenario, with each definition found through lookup
// and held to its pin (SCN-6: a definition is missing, or differs from an
// exact pin), or nullopt with the findings.
[[nodiscard]] std::optional<Roster>
scenario_roster(const Scenario& scenario, const DefinitionLookup& lookup,
                Findings& findings);

// Sets up a new game for the scenario: roster, start position, mode, size
// and difficulty in config, and config.rules as base_rules (the player's
// options) with the scenario's game_rules on top. False, with a finding,
// when the game cannot start this way (SCN-7: more locations than this
// build simulates at once).
bool configure_new_game(const std::shared_ptr<const Scenario>& scenario,
                        const Roster& roster, init::Config& config,
                        const GameRules& base_rules, Findings& findings);

} // namespace core::content
