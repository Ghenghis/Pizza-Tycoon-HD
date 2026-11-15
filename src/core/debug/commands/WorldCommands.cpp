/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/debug/commands/WorldCommands.h"

#include <charconv>
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

#include "core/content/AreaCatalog.h"
#include "core/content/AreaGenerator.h"
#include "core/content/ContentKey.h"
#include "core/content/Findings.h"
#include "core/content/Roster.h"
#include "core/content/ShippedScenarios.h"
#include "core/state/GameState.h"
#include "core/util/GameContext.h"
#include "engine/FormatCompat.h"

namespace debug::commands {

using debug::command::ArgSpec;
using debug::command::ArgType;
using debug::command::CommandContext;
using debug::command::CommandDef;
using debug::command::CommandResult;
using debug::command::DefaultProvider;

namespace {

// The shipped home location of each territory that has one.
std::optional<std::string_view> shipped_home(std::string_view territory) {
  if (territory == "north-pole") {
    return "pthd:santas-pizza-village";
  }
  return std::nullopt;
}

std::string page_line(const core::content::AreaPage& p) {
  return compat::format(
      "{:>2}. {:<24} {:<12} {}{}", p.page, p.name,
      p.area_id.empty() ? std::string("home") : p.area_id, p.key.str(),
      p.state == core::content::AreaState::Draft ? "  (draft)" : "");
}

} // namespace

bool ensure_territory(GameState& state, std::string_view territory,
                      std::string* why) {
  auto& catalog = core::content::area_catalog();
  if (!catalog.has_territory(territory)) {
    // The game's own copy first: its pinned revision is the style it plays.
    for (const auto& e : state.roster().entries()) {
      if (e.definition &&
          core::content::territory_of(*e.definition) == territory) {
        catalog.add_territory(e.definition);
        break;
      }
    }
  }
  if (!catalog.has_territory(territory)) {
    if (const auto key = shipped_home(territory)) {
      core::content::Findings findings;
      if (auto home = core::content::find_shipped_location(
              *core::content::ContentKey::parse(*key), findings)) {
        catalog.add_territory(std::move(home));
      }
    }
  }
  if (!catalog.has_territory(territory)) {
    if (why) {
      *why = "no territory '" + std::string(territory) +
             "': this build knows north-pole, and any territory of the game "
             "in progress";
    }
    return false;
  }
  // Areas of the territory the game holds but this catalog has not seen.
  const auto home = catalog.home(territory);
  for (const auto& e : state.roster().entries()) {
    if (e.definition && e.definition->key != home->key &&
        core::content::territory_of(*e.definition) == territory) {
      (void)catalog.adopt(e.definition);
    }
  }
  return true;
}

std::optional<core::CityId> enter_area(GameState& state, GameContext& context,
                                       std::string_view id_or_key,
                                       std::string* why, bool* added) {
  const auto fail = [why](std::string message) -> std::optional<core::CityId> {
    if (why) {
      *why = std::move(message);
    }
    return std::nullopt;
  };
  if (added) {
    *added = false;
  }
  // A console run with no game yet gets the minimal one first (as `nav
  // cityview` does); a game in progress is left as it is.
  state.bootstrap_minimal_playable_world(context);
  // Make the game's own territories known first (adopting their areas).
  for (const auto& e : state.roster().entries()) {
    if (e.definition) {
      (void)ensure_territory(state, core::content::territory_of(*e.definition),
                             nullptr);
    }
  }
  auto& catalog = core::content::area_catalog();
  const core::content::Area* area = catalog.find(id_or_key);
  std::optional<core::content::ContentKey> key;
  if (area) {
    key = area->key;
  }
  else {
    key = core::content::ContentKey::parse(id_or_key);
    if (!key) {
      return fail("no area " + std::string(id_or_key) +
                  "; `world areas <territory>` lists them");
    }
    area = catalog.find_by_key(*key);
  }

  // A place the game already holds: its pinned revision, whatever the
  // catalog has since.
  std::optional<core::CityId> city;
  const auto& entries = state.roster().entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].key == *key) {
      city = core::CityId{static_cast<int>(i)};
      break;
    }
  }
  if (!city) {
    if (!area) {
      return fail("no area " + std::string(id_or_key) +
                  "; `world areas <territory>` lists them");
    }
    if (area->state != core::content::AreaState::Admitted ||
        !area->definition) {
      std::string message = area->name + " (" + area->id +
                            ") is a draft and cannot be played yet:";
      for (const auto& m : catalog.play_problems(area->id)) {
        message += "\n  - " + m;
      }
      return fail(std::move(message));
    }
    std::string refused;
    if (!state.expand_roster(
            core::content::RosterEntry{
                .key = area->key,
                .source = core::content::RosterSource::Definition,
                .classic_number = std::nullopt,
                .definition = area->definition},
            &refused)) {
      return fail(area->name + " cannot join this game: " + refused);
    }
    city = core::CityId{static_cast<int>(state.roster().size()) - 1};
    if (added) {
      *added = true;
    }
  }
  state.ensure_city_loaded(*city);
  return city;
}

static CommandDef make_world_add_area() {
  return CommandDef{
      .name = "add-area",
      .description = "Add an area to a territory, in its style",
      .help_text =
          R"(Usage: world add-area <territory> <name> [seed] [auto|empty]

Adds a new local area to a territory, in the style of its home location
(its kit, districts, suppliers and art). auto (the default) generates a
playable area: roads, premises, districts, suppliers and demand, checked
with the location rules. empty starts a draft in the same style that lists
what it still needs before Play. The same seed gives the same area. A
failure changes nothing. The game in progress is not changed: `nav area`
takes it to the new area.

Examples:
  world add-area north-pole "Frostfall Hollow"
  world add-area north-pole "Frostfall Hollow" 7
  world add-area north-pole "Blank Slate" empty
)",
      .args =
          {
              ArgSpec{.name = "territory",
                      .type = ArgType::String,
                      .optional = false,
                      .default_provider = DefaultProvider::None,
                      .description = "Territory id (north-pole)"},
              ArgSpec{.name = "name",
                      .type = ArgType::String,
                      .optional = false,
                      .default_provider = DefaultProvider::None,
                      .description = "Area name (quote it with spaces)"},
              ArgSpec{.name = "seed",
                      .type = ArgType::String,
                      .optional = true,
                      .default_provider = DefaultProvider::None,
                      .description = "Seed, or the mode"},
              ArgSpec{.name = "mode",
                      .type = ArgType::String,
                      .optional = true,
                      .default_provider = DefaultProvider::None,
                      .description = "auto or empty"},
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const std::string territory = ctx.string_arg(0);
        const std::string name = ctx.string_arg(1);
        std::optional<std::uint64_t> seed;
        core::content::AreaMode mode = core::content::AreaMode::Automatic;
        for (std::size_t i = 2; i < 4; ++i) {
          if (!ctx.has_arg(i)) {
            continue;
          }
          const std::string word = ctx.string_arg(i);
          if (word == "auto") {
            mode = core::content::AreaMode::Automatic;
            continue;
          }
          if (word == "empty") {
            mode = core::content::AreaMode::Empty;
            continue;
          }
          std::uint64_t value = 0;
          const auto [ptr, ec] =
              std::from_chars(word.data(), word.data() + word.size(), value);
          if (ec != std::errc{} || ptr != word.data() + word.size()) {
            return CommandResult::error(compat::format(
                "Expected a seed, auto or empty, got '{}'", word));
          }
          seed = value;
        }
        std::string why;
        if (!ensure_territory(ctx.state, territory, &why)) {
          return CommandResult::error(why);
        }
        const auto result =
            core::content::area_catalog().add_area(territory, name, seed, mode);
        if (!result.area) {
          return CommandResult::error(
              "No area was added; the world is unchanged",
              core::content::describe(result.findings));
        }
        const auto& a = *result.area;
        CommandResult out = CommandResult::success();
        out.add_message(compat::format(
            "Added {} as {} ({}, {}, seed {})", a.name, a.id, a.key.str(),
            core::content::area_state_name(a.state), a.seed));
        if (!result.missing.empty()) {
          out.add_message("Before Play it needs:");
          for (const auto& m : result.missing) {
            out.add_message("  - " + m);
          }
        }
        return out;
      },
  };
}

static CommandDef make_world_areas() {
  return CommandDef{
      .name = "areas",
      .description = "List a territory's areas, in page order",
      .help_text = R"(Usage: world areas <territory>

Lists the territory's pages: its home location, then its areas in their
order, with their ids and location keys. Page numbers are presentation only;
an area keeps its id and key.

Example:
  world areas north-pole
)",
      .args =
          {
              ArgSpec{.name = "territory",
                      .type = ArgType::String,
                      .optional = false,
                      .default_provider = DefaultProvider::None,
                      .description = "Territory id (north-pole)"},
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const std::string territory = ctx.string_arg(0);
        std::string why;
        if (!ensure_territory(ctx.state, territory, &why)) {
          return CommandResult::error(why);
        }
        CommandResult out = CommandResult::success();
        for (const auto& p : core::content::area_catalog().pages(territory)) {
          out.add_message(page_line(p));
        }
        return out;
      },
  };
}

static CommandDef make_world_area_rename() {
  return CommandDef{
      .name = "area-rename",
      .description = "Rename an area",
      .help_text = R"(Usage: world area-rename <id> <name>

Gives an area a new name. Its id, its location key and everything in it
stay; its document gets the next revision. A game that already holds the
area keeps the revision it has.

Example:
  world area-rename area-000001 "Frostfall Harbour"
)",
      .args =
          {
              ArgSpec{.name = "id",
                      .type = ArgType::String,
                      .optional = false,
                      .default_provider = DefaultProvider::None,
                      .description = "Area id (area-000001)"},
              ArgSpec{.name = "name",
                      .type = ArgType::String,
                      .optional = false,
                      .default_provider = DefaultProvider::None,
                      .description = "New name (quote it with spaces)"},
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const std::string id = ctx.string_arg(0);
        const std::string name = ctx.string_arg(1);
        std::string why;
        if (!core::content::area_catalog().rename(id, name, &why)) {
          return CommandResult::error(why);
        }
        return CommandResult::success().add_message(
            compat::format("{} is now {}", id, name));
      },
  };
}

CommandDef make_world_commands() {
  return CommandDef{
      .name = "world",
      .description = "Territories and their areas",
      .help_text = "Add, list and rename the areas of a territory",
  }
      .add_subcommand(make_world_add_area())
      .add_subcommand(make_world_areas())
      .add_subcommand(make_world_area_rename());
}

} // namespace debug::commands
