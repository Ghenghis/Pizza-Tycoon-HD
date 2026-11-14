/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/debug/command/CommandRegistry.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>

#include "CommandContext.h"
#include "core/debug/command/CommandTokenizer.h"
#include "core/state/GameState.h"
#include "core/util/GameContext.h"
#include "engine/Logger.h"

namespace debug::command {

// Argument to 'help' that dumps every command and subcommand.
constexpr std::string_view all_keyword = "all";

// Argument to 'help' that lists every command with its arguments.
constexpr std::string_view schema_keyword = "schema";

static void sort_by_name(std::vector<const CommandDef*>& cmds) {
  std::ranges::sort(cmds, {}, &CommandDef::name);
}

static std::vector<const CommandDef*>
sorted_subcommands(const CommandDef& cmd) {
  std::vector<const CommandDef*> subs;
  subs.reserve(cmd.subcommands.size());
  for (const auto& [name, subcmd] : cmd.subcommands) {
    subs.push_back(&subcmd);
  }
  sort_by_name(subs);
  return subs;
}

// The words a command answers to: its name, then its aliases.
static std::vector<std::string_view> words_of(const CommandDef& cmd) {
  std::vector<std::string_view> words;
  words.reserve(cmd.aliases.size() + 1);
  words.emplace_back(cmd.name);
  for (const auto& alias : cmd.aliases) {
    words.emplace_back(alias);
  }
  return words;
}

// The same word twice in one command's own name and aliases.
static std::optional<std::string> repeated_word(const CommandDef& cmd,
                                                const std::string& path) {
  const auto words = words_of(cmd);
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (words[i].empty()) {
      return compat::format("'{}' has an empty name or alias", path);
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (words[i] == words[j]) {
        return compat::format("'{}' lists '{}' twice", path, words[i]);
      }
    }
  }
  return std::nullopt;
}

// The first word in a command's tree that would answer for two subcommands
// of one command, described by its path; nothing when the tree is clean.
static std::optional<std::string> find_clash(const CommandDef& cmd,
                                             const std::string& path) {
  if (!cmd.duplicate_subcommands.empty()) {
    return compat::format("'{}' has two subcommands called '{}'", path,
                          cmd.duplicate_subcommands.front());
  }

  const auto subs = sorted_subcommands(cmd);
  std::unordered_map<std::string_view, std::string_view> owner;
  for (const auto* sub : subs) {
    const std::string sub_path = compat::format("{} {}", path, sub->name);
    if (auto repeated = repeated_word(*sub, sub_path)) {
      return repeated;
    }
    for (const auto word : words_of(*sub)) {
      const auto [it, added] = owner.try_emplace(word, sub->name);
      if (!added) {
        return compat::format("in '{}', '{}' would mean both '{}' and '{}'",
                              path, word, it->second, sub->name);
      }
    }
  }

  for (const auto* sub : subs) {
    if (auto clash =
            find_clash(*sub, compat::format("{} {}", path, sub->name))) {
      return clash;
    }
  }
  return std::nullopt;
}

// Where an omitted argument's value comes from, in words.
static std::string_view default_source(DefaultProvider provider) {
  switch (provider) {
  case DefaultProvider::None:
    return "";
  case DefaultProvider::ActivePlayer:
    return "active player";
  case DefaultProvider::ActiveRestaurant:
    return "active restaurant";
  case DefaultProvider::ActiveCity:
    return "active city";
  case DefaultProvider::CurrentDate:
    return "current date";
  }
  return "";
}

static ArgSchema describe_arg(const ArgSpec& spec) {
  return ArgSchema{
      .name = spec.name,
      .type = std::string(type_name(spec.type)),
      .optional = spec.optional,
      .default_from = std::string(default_source(spec.default_provider)),
      .min_value = spec.min_value,
      .max_value = spec.max_value,
      .allowed_values =
          spec.allowed_values.value_or(std::vector<std::string>{}),
      .custom_check = spec.custom_validator.has_value(),
      .description = spec.description,
  };
}

static void collect_schemas(const CommandDef& cmd, const std::string& path,
                            std::vector<CommandSchema>& out) {
  CommandSchema schema{
      .path = path,
      .aliases = cmd.aliases,
      .description = cmd.description,
      .runnable = cmd.is_leaf(),
      .args = {},
  };
  schema.args.reserve(cmd.args.size());
  for (const auto& arg : cmd.args) {
    schema.args.push_back(describe_arg(arg));
  }
  out.push_back(std::move(schema));

  for (const auto* sub : sorted_subcommands(cmd)) {
    collect_schemas(*sub, compat::format("{} {}", path, sub->name), out);
  }
}

// "player: player ID, optional, default active player", then any limits:
// ", 0..100", ", one of a|b", ", checked by the command".
static std::string format_arg_schema(const ArgSchema& arg) {
  std::string text = compat::format("{}: {}", arg.name, arg.type);
  if (arg.optional) {
    text += ", optional";
  }
  if (!arg.default_from.empty()) {
    text += compat::format(", default {}", arg.default_from);
  }
  if (arg.min_value || arg.max_value) {
    text += ", ";
    if (arg.min_value) {
      text += compat::format("{}", *arg.min_value);
    }
    text += "..";
    if (arg.max_value) {
      text += compat::format("{}", *arg.max_value);
    }
  }
  if (!arg.allowed_values.empty()) {
    std::string joined;
    for (const auto& value : arg.allowed_values) {
      if (!joined.empty()) {
        joined += '|';
      }
      joined += value;
    }
    text += compat::format(", one of {}", joined);
  }
  if (arg.custom_check) {
    text += ", checked by the command";
  }
  return text;
}

// "restaurant (r, rest)", or just the name when there are no aliases.
static std::string label_with_aliases(const CommandDef& cmd) {
  if (cmd.aliases.empty()) {
    return cmd.name;
  }

  std::string joined;
  for (const auto& alias : cmd.aliases) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += alias;
  }
  return compat::format("{} ({})", cmd.name, joined);
}

// Mirror a command's console output to the log as one multi-line message, so
// the body copies out of a terminal without a timestamp on every line.
static void log_output(const std::vector<std::string>& tokens,
                       const CommandResult& result) {
  std::string text = ">";
  for (const auto& token : tokens) {
    text += ' ';
    text += token;
  }
  for (const auto& message : result.messages) {
    text += '\n';
    text += message;
  }
  if (result.error_message) {
    text += '\n';
    text += *result.error_message;
  }
  Logger::info("{}", text);
}

std::optional<std::string> CommandRegistry::register_command(CommandDef cmd) {
  std::optional<std::string> problem = repeated_word(cmd, cmd.name);
  if (!problem) {
    for (const auto word : words_of(cmd)) {
      const std::string key(word);
      if (commands_.contains(key)) {
        problem = compat::format("'{}' is already a command", word);
        break;
      }
      if (const auto it = aliases_.find(key); it != aliases_.end()) {
        problem = compat::format("'{}' is already an alias of '{}'", word,
                                 it->second);
        break;
      }
    }
  }
  if (!problem) {
    problem = find_clash(cmd, cmd.name);
  }

  if (problem) {
    std::string error = compat::format("Command '{}' was not registered: {}",
                                       cmd.name, *problem);
    Logger::warn("{}", error);
    registration_errors_.push_back(error);
    return error;
  }

  const std::string name = cmd.name;
  for (const auto& alias : cmd.aliases) {
    aliases_.emplace(alias, name);
  }
  commands_.emplace(name, std::move(cmd));
  return std::nullopt;
}

std::vector<CommandSchema> CommandRegistry::list_commands() const {
  std::vector<const CommandDef*> roots;
  roots.reserve(commands_.size());
  for (const auto& [name, cmd] : commands_) {
    roots.push_back(&cmd);
  }
  sort_by_name(roots);

  std::vector<CommandSchema> result;
  for (const auto* root : roots) {
    collect_schemas(*root, root->name, result);
  }
  return result;
}

std::optional<std::reference_wrapper<const CommandDef>>
CommandRegistry::find_command(const std::string& name) const {
  // Try direct lookup
  auto it = commands_.find(name);
  if (it != commands_.end()) {
    return std::cref(it->second);
  }

  // Try alias lookup
  auto alias_it = aliases_.find(name);
  if (alias_it != aliases_.end()) {
    auto cmd_it = commands_.find(alias_it->second);
    if (cmd_it != commands_.end()) {
      return std::cref(cmd_it->second);
    }
  }

  return std::nullopt;
}

std::vector<std::string> CommandRegistry::get_all_command_names() const {
  std::vector<std::string> result;

  for (const auto& [name, cmd] : commands_) {
    result.push_back(name);
  }

  return result;
}

std::vector<std::string>
CommandRegistry::get_help(const std::string& command_path) const {
  std::vector<std::string> result;

  if (command_path == schema_keyword) {
    for (const auto& cmd : list_commands()) {
      std::string line = cmd.path;
      if (!cmd.aliases.empty()) {
        std::string joined;
        for (const auto& alias : cmd.aliases) {
          if (!joined.empty()) {
            joined += ", ";
          }
          joined += alias;
        }
        line += compat::format(" ({})", joined);
      }
      line += cmd.runnable ? " - " : " [group] - ";
      line += cmd.description;
      result.push_back(std::move(line));
      for (const auto& arg : cmd.args) {
        result.push_back("    " + format_arg_schema(arg));
      }
    }
    return result;
  }

  if (command_path.empty() || command_path == all_keyword) {
    const bool full_tree = (command_path == all_keyword);

    std::vector<const CommandDef*> cmds;
    cmds.reserve(commands_.size());
    for (const auto& [name, cmd] : commands_) {
      cmds.push_back(&cmd);
    }
    sort_by_name(cmds);

    std::vector<std::string> labels;
    labels.reserve(cmds.size());
    std::size_t width = 0;
    for (const auto* cmd : cmds) {
      labels.push_back(label_with_aliases(*cmd));
      width = std::max(width, labels.back().size());
    }

    result.push_back("Available commands:");
    for (std::size_t i = 0; i < cmds.size(); ++i) {
      result.push_back(compat::format("  {:<{}}  {}", labels[i], width,
                                      cmds[i]->description));
      if (full_tree) {
        collect_subcommand_help(*cmds[i], "  " + cmds[i]->name, result);
      }
    }

    if (!full_tree) {
      result.push_back(
          compat::format("Use 'help <command>', 'help {}' for every "
                         "subcommand, or 'help {}' for every argument",
                         all_keyword, schema_keyword));
    }
  }
  else {
    // Specific command help
    auto cmd_ref = find_command(command_path);
    if (!cmd_ref) {
      result.push_back(compat::format("Unknown command: {}", command_path));
      return result;
    }

    const auto& cmd = cmd_ref->get();
    auto cmd_help = format_command_help(cmd);
    result.insert(result.end(), cmd_help.begin(), cmd_help.end());

    if (!cmd.subcommands.empty()) {
      result.push_back("Subcommands:");
      collect_subcommand_help(cmd, cmd.name, result);
    }
  }

  return result;
}

CommandResult CommandRegistry::execute(const std::string& line,
                                       GameState& state, GameContext& context) {
  return execute(CommandTokenizer::tokenize_simple(line), state, context);
}

CommandResult CommandRegistry::execute(const std::vector<std::string>& tokens,
                                       GameState& state, GameContext& context) {
  if (tokens.empty()) {
    return CommandResult::invalid_syntax("No command entered");
  }

  // Find the root command
  auto cmd_ref = find_command(tokens[0]);
  if (!cmd_ref) {
    auto result = CommandResult::invalid_syntax(
        compat::format("Unknown command: '{}'", tokens[0]),
        "Try 'help' for a list of commands");
    Logger::warn("Command error: {}", *result.error_message);
    return result;
  }

  const CommandDef& root = cmd_ref->get();

  // Parse the command and arguments
  auto parsed = parser_.parse(root, tokens, state);
  if (!parsed.success) {
    // Log the parsing error so user knows why it failed
    if (parsed.error_result.error_message) {
      Logger::warn("Command parsing failed: {}",
                   *parsed.error_result.error_message);
      if (parsed.error_result.error_suggestion) {
        Logger::warn("  Suggestion: {}", *parsed.error_result.error_suggestion);
      }
    }
    return parsed.error_result;
  }

  // Execute the command handler
  try {
    CommandContext ctx{state, context, *this, std::move(parsed.args)};
    CommandResult result = parsed.command->handler(ctx);
    log_output(tokens, result);
    return result;
  }
  catch (const std::exception& e) {
    auto result = CommandResult::error(
        compat::format("Command execution failed: {}", e.what()));
    Logger::warn("Command execution error: {}", e.what());
    return result;
  }
}

std::vector<std::string>
CommandRegistry::format_command_help(const CommandDef& cmd) const {
  std::vector<std::string> result;

  // Command name and description
  result.push_back(compat::format("Command: {}", cmd.name));
  if (!cmd.aliases.empty()) {
    std::string aliases_str;
    for (size_t i = 0; i < cmd.aliases.size(); ++i) {
      if (i > 0)
        aliases_str += ", ";
      aliases_str += cmd.aliases[i];
    }
    result.push_back(compat::format("Aliases: {}", aliases_str));
  }
  result.push_back(compat::format("Description: {}", cmd.description));
  result.push_back("");

  // Arguments
  if (!cmd.args.empty()) {
    result.push_back("Arguments:");
    for (const auto& arg : cmd.args) {
      std::string arg_desc = compat::format(
          "  {} ({}) - {}", arg.name, type_name(arg.type), arg.description);
      if (arg.optional) {
        arg_desc += " [optional]";
      }
      result.push_back(arg_desc);
    }
    result.push_back("");
  }

  // Subcommands
  if (!cmd.subcommands.empty()) {
    result.push_back("Subcommands:");
    for (const auto& [name, subcmd] : cmd.subcommands) {
      result.push_back(compat::format("  {} - {}", name, subcmd.description));
    }
    result.push_back("");
  }

  // Detailed help text
  if (!cmd.help_text.empty()) {
    result.push_back(cmd.help_text);
  }

  return result;
}

void CommandRegistry::collect_subcommand_help(
    const CommandDef& cmd, const std::string& prefix,
    std::vector<std::string>& output) const {
  for (const auto* subcmd : sorted_subcommands(cmd)) {
    std::string full_name = prefix.empty()
                                ? subcmd->name
                                : compat::format("{} {}", prefix, subcmd->name);
    output.push_back(compat::format("{} - {}", full_name, subcmd->description));

    // Recurse for nested subcommands
    if (!subcmd->subcommands.empty()) {
      collect_subcommand_help(*subcmd, full_name, output);
    }
  }
}
} // namespace debug::command
