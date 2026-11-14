/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/ShippedScenarios.h"

#include <algorithm>
#include <system_error>
#include <vector>

#include "core/content/LocationValidator.h"
#include "core/content/Roster.h"
#include "core/content/Scenario.h"
#include "core/factories/CharacterFactory.h"
#include "core/init/CompanyNames.h"
#include "core/init/NewGameConfig.h"
#include "core/state/GameRules.h"
#include "core/state/GameState.h"
#include "core/util/Constants.h"
#include "engine/PlatformPaths.h"
#include "engine/Random.h"

namespace core::content {

namespace fs = std::filesystem;

fs::path shipped_data_dir() { return fs::path(platform_paths::data_dir()); }

std::shared_ptr<const LocationDefinition>
find_shipped_location(const ContentKey& key, Findings& findings,
                      const fs::path& data_dir) {
  const fs::path root = data_dir / "locations";
  std::error_code ec;
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".json") {
      files.push_back(it->path());
    }
  }
  // Same answer on every system, whatever order the folder lists in.
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    auto report = load_location(file);
    if (report.ok() && report.definition->key == key) {
      return report.definition;
    }
    if (!report.ok() && report.definition == nullptr &&
        file.stem().string() == std::string(key.slug())) {
      findings.insert(findings.end(), report.findings.begin(),
                      report.findings.end());
    }
  }
  findings.push_back(Finding{.rule = "SCN-6",
                             .severity = Severity::Error,
                             .pointer = "",
                             .message = "no valid definition of " + key.str() +
                                        " in " + root.string()});
  return nullptr;
}

bool prepare_shipped_scenario(std::string_view file, init::Config& config,
                              const GameRules& base_rules, Findings& findings,
                              const fs::path& data_dir) {
  const fs::path path = data_dir / "scenarios" / (std::string(file) + ".json");
  auto read = read_scenario_file(path);
  findings.insert(findings.end(), read.findings.begin(), read.findings.end());
  if (!read.ok()) {
    return false;
  }
  const auto roster = scenario_roster(
      *read.scenario,
      [&](const ContentKey& key) {
        return find_shipped_location(key, findings, data_dir);
      },
      findings);
  if (!roster) {
    return false;
  }
  return configure_new_game(read.scenario, *roster, config, base_rules,
                            findings);
}

bool start_shipped_scenario(GameState& state, GameContext& context,
                            std::string_view file,
                            std::optional<std::uint32_t> seed,
                            Findings& findings) {
  state.begin_initialization();
  auto& config = state.initializer();
  if (!prepare_shipped_scenario(file, config, state.options().rules,
                                findings)) {
    return false;
  }
  if (seed) {
    engine::random::seed(*seed);
  }
  config.players.resize(NUMBER_OF_PLAYERS);
  const auto company_names = init::random_company_names();
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    const auto* character = CharacterFactory::create_character(i);
    config.players[i].name = company_names[static_cast<std::size_t>(i)];
    config.players[i].character_id = i;
    config.players[i].character_name = character->name();
    config.players[i].is_human = i == 0;
  }
  state.finalize_initialization(context);
  return true;
}

} // namespace core::content
