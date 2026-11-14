/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "CommandDef.h"
#include "CommandParser.h"
#include "CommandResult.h"

// Forward declarations
class GameState;
class GameContext;

namespace debug::command {

// One argument of a command, as the console reads it.
struct ArgSchema {
  std::string name;
  std::string type; // type_name(), e.g. "player ID"
  bool optional = false;
  std::string default_from; // Where an omitted value comes from, or empty
  std::optional<int64_t> min_value;
  std::optional<int64_t> max_value;
  std::vector<std::string> allowed_values;
  bool custom_check = false; // The command checks the value itself too
  std::string description;
};

// One command or subcommand, named by the words that reach it.
struct CommandSchema {
  std::string path;                 // e.g. "player money set"
  std::vector<std::string> aliases; // Other words for the last one
  std::string description;
  bool runnable = false; // Has a handler; otherwise only groups subcommands
  std::vector<ArgSchema> args;
};

// Command registry - central repository for all commands
class CommandRegistry {
public:
  // Register a command tree under its name and aliases. The tree is refused,
  // and the registry left as it was, when a word in it would answer for two
  // commands: its name or an alias is already registered, or two subcommands
  // of one command share a name or an alias. The reason is returned, logged
  // and kept in registration_errors().
  std::optional<std::string> register_command(CommandDef cmd);

  // Every refusal so far, oldest first.
  const std::vector<std::string>& registration_errors() const {
    return registration_errors_;
  }

  // Every registered command and subcommand with its arguments, sorted by
  // path.
  std::vector<CommandSchema> list_commands() const;

  // Find a command by name or alias
  std::optional<std::reference_wrapper<const CommandDef>>
  find_command(const std::string& name) const;

  // Get all command names (for completion)
  std::vector<std::string> get_all_command_names() const;

  // Get all aliases and their target command names
  const std::unordered_map<std::string, std::string>& get_aliases() const {
    return aliases_;
  }

  // Generate help text: a command's help, "all" for the whole tree, or
  // "schema" for every command with its arguments
  std::vector<std::string> get_help(const std::string& command_path = "") const;

  // Execute a command from a string; the line is split into tokens, honouring
  // quotes and backslash escapes
  CommandResult execute(const std::string& line, GameState& state,
                        GameContext& context);

  // Execute a command from already separated arguments, e.g. argv
  CommandResult execute(const std::vector<std::string>& tokens,
                        GameState& state, GameContext& context);

private:
  std::unordered_map<std::string, CommandDef> commands_;
  std::unordered_map<std::string, std::string> aliases_; // alias -> real name
  std::vector<std::string> registration_errors_;

  CommandParser parser_;

  // Helper to format help for a command
  std::vector<std::string> format_command_help(const CommandDef& cmd) const;

  // Helper to collect subcommand help recursively
  void collect_subcommand_help(const CommandDef& cmd, const std::string& prefix,
                               std::vector<std::string>& output) const;
};

} // namespace debug::command
