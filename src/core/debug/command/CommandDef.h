/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ArgSpec.h"
#include "CommandContext.h"
#include "CommandResult.h"

namespace debug::command {

// Command handler function type
using CommandHandler = std::function<CommandResult(const CommandContext&)>;

// Command definition - supports hierarchical commands with subcommands
struct CommandDef {
  std::string name;                 // Primary command name
  std::vector<std::string> aliases; // Alternative names
  std::string description;          // Short description (one line)
  std::string help_text;            // Detailed help with examples
  std::vector<ArgSpec> args;        // Positional arguments
  std::unordered_map<std::string, CommandDef> subcommands; // Nested commands
  CommandHandler handler; // The actual implementation (leaf nodes only)
  // Subcommand names added more than once. The first one is kept, and the
  // registry refuses the whole tree, so the clash is never silent.
  std::vector<std::string> duplicate_subcommands = {};

  // Is this a leaf command (has handler) or just a namespace?
  bool is_leaf() const { return handler != nullptr; }

  // Fluent builder methods for constructing command trees
  CommandDef& add_subcommand(CommandDef subcmd) {
    std::string key = subcmd.name;
    if (!subcommands.try_emplace(key, std::move(subcmd)).second) {
      duplicate_subcommands.push_back(std::move(key));
    }
    return *this;
  }

  CommandDef& add_alias(std::string alias) {
    aliases.push_back(std::move(alias));
    return *this;
  }

  CommandDef& with_handler(CommandHandler h) {
    handler = std::move(h);
    return *this;
  }
};

} // namespace debug::command
