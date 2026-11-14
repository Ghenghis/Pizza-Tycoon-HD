/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/ScenarioSave.h"

#include <utility>
#include <vector>

#include "core/content/ClassicCities.h"
#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"

namespace core::content {

namespace {
using json = nlohmann::json;
constexpr std::string_view block_format = "pthd.save-scenario";
constexpr std::string_view contract_version = "1.0.0";
} // namespace

json save_scenario_block(const Roster& roster,
                         const std::shared_ptr<const Scenario>& scenario) {
  json block;
  block["format"] = block_format;
  block["contract_version"] = contract_version;
  block["scenario"] =
      scenario ? json::parse(scenario->canonical_document) : json(nullptr);
  json entries = json::array();
  for (std::size_t i = 0; i < roster.size(); ++i) {
    const RosterEntry& e = roster.entries()[i];
    json entry;
    entry["index"] = i;
    entry["location_key"] = e.key.str();
    if (e.source == RosterSource::Classic) {
      entry["source"] = "classic";
      entry["classic_number"] = *e.classic_number;
      entry["classic_source_sha256"] = nullptr;
    }
    else {
      entry["source"] = "embedded";
      entry["revision"] = e.definition->revision;
      entry["definition_sha256"] = e.definition->sha256;
      entry["definition"] = json::parse(e.definition->canonical_document);
    }
    entries.push_back(std::move(entry));
  }
  block["roster"] = std::move(entries);
  return block;
}

std::optional<SavedScenario> read_scenario_block(const json& block,
                                                 std::string& error) {
  const auto fail = [&](std::string message) -> std::optional<SavedScenario> {
    error = std::move(message);
    return std::nullopt;
  };
  if (!block.is_object() || block.value("format", "") != block_format) {
    return fail("the save's scenario block is not one this build reads");
  }
  if (block.value("contract_version", "") != contract_version) {
    return fail("the save was made with a newer version of the game");
  }
  const auto roster_it = block.find("roster");
  if (roster_it == block.end() || !roster_it->is_array()) {
    return fail("the save's roster is missing");
  }

  std::vector<RosterEntry> entries;
  for (std::size_t i = 0; i < roster_it->size(); ++i) {
    const json& e = (*roster_it)[i];
    const std::string where = "roster entry " + std::to_string(i);
    if (!e.is_object() || e.value("index", -1) != static_cast<int>(i)) {
      return fail(where + " is out of order");
    }
    const auto key = ContentKey::parse(e.value("location_key", ""));
    if (!key) {
      return fail(where + " has no location key");
    }
    const std::string source = e.value("source", "");
    if (source == "classic") {
      const auto id = classic_city_id(*key);
      if (!id || e.value("classic_number", -1) != id->value) {
        return fail(where + " names no original city");
      }
      entries.push_back(RosterEntry{.key = *key,
                                    .source = RosterSource::Classic,
                                    .classic_number = id->value,
                                    .definition = nullptr});
      continue;
    }
    if (source != "embedded" || !e.contains("definition")) {
      return fail(where + " has no embedded definition");
    }
    auto read = read_location(e.at("definition"));
    if (!read.definition) {
      return fail("the save's definition of " + key->str() + " is damaged:\n" +
                  describe(read.findings));
    }
    if (read.definition->sha256 != e.value("definition_sha256", "")) {
      return fail("the save's definition of " + key->str() +
                  " does not match its hash; the save is damaged");
    }
    if (const auto findings = validate_location(*read.definition);
        has_errors(findings)) {
      return fail("the save's definition of " + key->str() +
                  " breaks a rule:\n" + describe(findings));
    }
    if (read.definition->key != *key) {
      return fail(where + " holds the definition of another location");
    }
    entries.push_back(RosterEntry{.key = *key,
                                  .source = RosterSource::Definition,
                                  .classic_number = std::nullopt,
                                  .definition = std::move(read.definition)});
  }
  auto roster = Roster::from_entries(std::move(entries));
  if (!roster) {
    return fail("the save's roster breaks a roster rule");
  }

  std::shared_ptr<const Scenario> scenario;
  if (const auto it = block.find("scenario");
      it != block.end() && !it->is_null()) {
    auto read = read_scenario(*it);
    if (!read.ok()) {
      return fail("the save's scenario is damaged:\n" +
                  describe(read.findings));
    }
    scenario = std::move(read.scenario);
  }
  return SavedScenario{.roster = std::move(*roster),
                       .scenario = std::move(scenario)};
}

} // namespace core::content
