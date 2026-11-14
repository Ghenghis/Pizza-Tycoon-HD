/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The console's command registry: a word may answer for one command only, so
// a second command with a taken name or alias is refused, loudly, instead of
// replacing the first. And every real command can be listed with the type
// and limits of its arguments.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/debug/DebugCommands.h"
#include "core/debug/command/CommandDef.h"
#include "core/debug/command/CommandRegistry.h"

using debug::command::ArgSpec;
using debug::command::ArgType;
using debug::command::CommandContext;
using debug::command::CommandDef;
using debug::command::CommandRegistry;
using debug::command::CommandResult;
using debug::command::CommandSchema;
using debug::command::DefaultProvider;

namespace {

// A runnable command with a description to tell copies apart.
CommandDef leaf(std::string name, std::string description = "",
                std::vector<std::string> aliases = {}) {
  CommandDef cmd;
  cmd.name = std::move(name);
  cmd.aliases = std::move(aliases);
  cmd.description = std::move(description);
  cmd.handler = [](const CommandContext&) { return CommandResult::success(); };
  return cmd;
}

// A command that only groups subcommands.
CommandDef group(std::string name, std::vector<std::string> aliases = {}) {
  CommandDef cmd;
  cmd.name = std::move(name);
  cmd.aliases = std::move(aliases);
  return cmd;
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// The node a path of names reaches, walking the tree directly.
const CommandDef* walk(const CommandRegistry& registry,
                       const std::string& path) {
  std::istringstream words(path);
  std::string word;
  words >> word;
  const auto root = registry.find_command(word);
  if (!root) {
    return nullptr;
  }
  const CommandDef* node = &root->get();
  while (words >> word) {
    const auto it = node->subcommands.find(word);
    if (it == node->subcommands.end()) {
      return nullptr;
    }
    node = &it->second;
  }
  return node;
}

std::size_t count_nodes(const CommandDef& cmd) {
  std::size_t count = 1;
  for (const auto& [name, sub] : cmd.subcommands) {
    count += count_nodes(sub);
  }
  return count;
}

} // namespace

TEST_CASE("A second command with a taken name is refused",
          "[debug][commands]") {
  CommandRegistry registry;
  REQUIRE_FALSE(registry.register_command(leaf("alpha", "first")));

  const auto error = registry.register_command(leaf("alpha", "second"));
  REQUIRE(error);
  CHECK(contains(*error, "'alpha' is already a command"));
  REQUIRE(registry.registration_errors().size() == 1);
  CHECK(registry.registration_errors().front() == *error);

  // The first one is still the one that runs.
  const auto found = registry.find_command("alpha");
  REQUIRE(found);
  CHECK(found->get().description == "first");
}

TEST_CASE("A command answering to a taken alias is refused",
          "[debug][commands]") {
  CommandRegistry registry;
  REQUIRE_FALSE(registry.register_command(leaf("beta", "", {"b"})));

  SECTION("as its alias") {
    const auto error = registry.register_command(leaf("bravo", "", {"b"}));
    REQUIRE(error);
    CHECK(contains(*error, "'b' is already an alias of 'beta'"));
    CHECK_FALSE(registry.find_command("bravo"));
  }

  SECTION("as its name") {
    const auto error = registry.register_command(leaf("b"));
    REQUIRE(error);
    CHECK(contains(*error, "'b' is already an alias of 'beta'"));
  }

  SECTION("as an alias that is another command's name") {
    const auto error = registry.register_command(leaf("bee", "", {"beta"}));
    REQUIRE(error);
    CHECK(contains(*error, "'beta' is already a command"));
    CHECK_FALSE(registry.find_command("bee"));
  }

  SECTION("listed twice by the command itself") {
    const auto error = registry.register_command(leaf("gamma", "", {"g", "g"}));
    REQUIRE(error);
    CHECK(contains(*error, "'gamma' lists 'g' twice"));
  }

  // The alias still leads to the first command.
  const auto found = registry.find_command("b");
  REQUIRE(found);
  CHECK(found->get().name == "beta");
  CHECK(registry.registration_errors().size() == 1);
}

TEST_CASE("Two subcommands with one name refuse the whole tree",
          "[debug][commands]") {
  CommandRegistry registry;

  SECTION("directly under the command") {
    CommandDef root = group("player");
    root.add_subcommand(leaf("money", "first"))
        .add_subcommand(leaf("money", "second"));
    REQUIRE(root.duplicate_subcommands == std::vector<std::string>{"money"});
    CHECK(root.subcommands.at("money").description == "first");

    const auto error = registry.register_command(std::move(root));
    REQUIRE(error);
    CHECK(contains(*error, "'player' has two subcommands called 'money'"));
  }

  SECTION("deeper down") {
    CommandDef money = group("money");
    money.add_subcommand(leaf("set")).add_subcommand(leaf("set"));
    CommandDef root = group("player");
    root.add_subcommand(std::move(money));

    const auto error = registry.register_command(std::move(root));
    REQUIRE(error);
    CHECK(contains(*error, "'player money' has two subcommands called 'set'"));
  }

  CHECK_FALSE(registry.find_command("player"));
  CHECK(registry.registration_errors().size() == 1);
}

TEST_CASE("Sibling subcommands may not share an alias", "[debug][commands]") {
  CommandRegistry registry;

  SECTION("two aliases") {
    CommandDef root = group("player");
    root.add_subcommand(leaf("money", "", {"m"}))
        .add_subcommand(leaf("mood", "", {"m"}));
    const auto error = registry.register_command(std::move(root));
    REQUIRE(error);
    CHECK(contains(*error, "in 'player', 'm' would mean both"));
  }

  SECTION("an alias that is a sibling's name") {
    CommandDef root = group("player");
    root.add_subcommand(leaf("cash"))
        .add_subcommand(leaf("money", "", {"cash"}));
    const auto error = registry.register_command(std::move(root));
    REQUIRE(error);
    CHECK(contains(*error, "'cash' would mean both 'cash' and 'money'"));
  }

  SECTION("an empty name") {
    CommandDef root = group("player");
    root.add_subcommand(leaf(""));
    const auto error = registry.register_command(std::move(root));
    REQUIRE(error);
    CHECK(contains(*error, "empty name or alias"));
  }

  CHECK_FALSE(registry.find_command("player"));
}

TEST_CASE("The same word under different commands is fine",
          "[debug][commands]") {
  CommandRegistry registry;
  CommandDef player = group("player", {"p"});
  player.add_subcommand(leaf("set", "", {"s"}));
  CommandDef game = group("game");
  game.add_subcommand(leaf("set", "", {"s"}));
  CommandDef nested = group("money");
  nested.add_subcommand(leaf("set", "", {"s"}));
  game.add_subcommand(std::move(nested));

  CHECK_FALSE(registry.register_command(std::move(player)));
  CHECK_FALSE(registry.register_command(std::move(game)));
  CHECK(registry.registration_errors().empty());
  CHECK(walk(registry, "game money set"));
}

TEST_CASE("Every real console command registers without a clash",
          "[debug][commands]") {
  CommandRegistry registry;
  register_core_commands(registry);

  // A refusal here names the clash; fix the command, not this test.
  for (const auto& error : registry.registration_errors()) {
    UNSCOPED_INFO(error);
  }
  REQUIRE(registry.registration_errors().empty());

  const auto names = registry.get_all_command_names();
  REQUIRE_FALSE(names.empty());

  // The listing has every command and subcommand exactly once.
  const std::vector<CommandSchema> listing = registry.list_commands();
  std::size_t nodes = 0;
  for (const auto& name : names) {
    nodes += count_nodes(registry.find_command(name)->get());
  }
  CHECK(listing.size() == nodes);

  std::set<std::string> paths;
  std::size_t runnable = 0;
  for (const auto& entry : listing) {
    CHECK(paths.insert(entry.path).second);

    // Each entry is what its path reaches in the tree.
    const CommandDef* node = walk(registry, entry.path);
    REQUIRE(node);
    CHECK(entry.runnable == node->is_leaf());
    CHECK(entry.aliases == node->aliases);
    REQUIRE(entry.args.size() == node->args.size());
    for (const auto& arg : entry.args) {
      CHECK_FALSE(arg.name.empty());
      CHECK_FALSE(arg.type.empty());
    }
    if (entry.runnable) {
      ++runnable;
    }
    else {
      // A group without a handler must lead somewhere.
      CHECK_FALSE(node->subcommands.empty());
    }
  }
  CHECK(std::ranges::is_sorted(listing, {}, &CommandSchema::path));
  CHECK(runnable > names.size());

  // The commands the help text and the tools rely on are there.
  for (const char* path : {"help", "dump", "debug dump fingerprint"}) {
    INFO(path);
    CHECK(paths.contains(path));
  }

  // 'help schema' prints one line per command, then its arguments.
  const auto lines = registry.get_help("schema");
  std::size_t command_lines = 0;
  for (const auto& line : lines) {
    if (!line.starts_with("    ")) {
      ++command_lines;
    }
  }
  CHECK(command_lines == listing.size());
}

TEST_CASE("The listing gives each argument's type and limits",
          "[debug][commands]") {
  ArgSpec amount = ArgSpec::required("amount", ArgType::Money, "How much");
  amount.min_value = 0;
  amount.max_value = 100;
  ArgSpec who = ArgSpec::optional_with_default(
      "player", ArgType::PlayerId, DefaultProvider::ActivePlayer, "Who");
  ArgSpec mode = ArgSpec::required("mode", ArgType::String, "How");
  mode.allowed_values = std::vector<std::string>{"add", "set"};

  CommandDef cmd = leaf("give", "Give money", {"g"});
  cmd.args = {amount, who, mode};

  CommandRegistry registry;
  REQUIRE_FALSE(registry.register_command(std::move(cmd)));

  const auto listing = registry.list_commands();
  REQUIRE(listing.size() == 1);
  const CommandSchema& give = listing.front();
  CHECK(give.path == "give");
  CHECK(give.aliases == std::vector<std::string>{"g"});
  CHECK(give.runnable);
  REQUIRE(give.args.size() == 3);

  CHECK(give.args[0].type == "money amount");
  CHECK_FALSE(give.args[0].optional);
  CHECK(give.args[0].min_value == 0);
  CHECK(give.args[0].max_value == 100);
  CHECK(give.args[1].optional);
  CHECK(give.args[1].default_from == "active player");
  CHECK(give.args[2].allowed_values == std::vector<std::string>{"add", "set"});

  const auto lines = registry.get_help("schema");
  REQUIRE(lines.size() == 4);
  CHECK(lines[0] == "give (g) - Give money");
  CHECK(contains(lines[1], "amount: money amount, 0..100"));
  CHECK(
      contains(lines[2], "player: player ID, optional, default active player"));
  CHECK(contains(lines[3], "mode: string, one of add|set"));
}
