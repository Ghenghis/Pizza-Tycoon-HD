/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/debug/RecordCommands.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

#include "engine/Game.h"
#include "engine/PlatformPaths.h"

using debug::command::ArgSpec;
using debug::command::ArgType;
using debug::command::CommandContext;
using debug::command::CommandDef;
using debug::command::CommandResult;

namespace engine {

namespace {

// Seconds from 0.5 to 3600, or nullopt.
std::optional<double> seconds_of(const std::string& text) {
  char* end = nullptr;
  const double s = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(s) || s < 0.5 ||
      s > 3600.0) {
    return std::nullopt;
  }
  return s;
}

std::string folder_arg(const CommandContext& ctx, std::size_t index) {
  return ctx.has_arg(index) ? ctx.string_arg(index)
                            : platform_paths::recordings_dir();
}

CommandResult lines(const std::string& text, bool ok = true) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    out.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return ok ? CommandResult::success(std::move(out))
            : CommandResult::error(text);
}

} // namespace

CommandDef make_record_command(Game& game, GameState& state) {
  CommandDef record{
      .name = "record",
      .description = "Record the game's picture and sound",
      .help_text = R"(Records what the game shows and plays (nothing else on the
computer) with FFmpeg, into Matroska segments and a recording.json, in a new
folder under the recordings folder. FFmpeg must be on the PATH, or named by
PLEGACY_FFMPEG. Classic picture (HD off) only, for now.

Examples:
  record start            # until record stop
  record start 30         # 30 seconds
  record stop
  record status
  record clip 5 /tmp/rec  # batch mode (--exec): 5 s of the current screen
)",
  };
  record.add_subcommand(CommandDef{
      .name = "start",
      .description = "Start recording",
      .args = {ArgSpec{.name = "seconds",
                       .type = ArgType::String,
                       .optional = true,
                       .description = "Stop by itself after this long"},
               ArgSpec{.name = "folder",
                       .type = ArgType::String,
                       .optional = true,
                       .description = "Where the recording's folder goes"}},
      .handler = [&game](const CommandContext& ctx) -> CommandResult {
        std::optional<double> seconds;
        if (ctx.has_arg(0)) {
          seconds = seconds_of(ctx.string_arg(0));
          if (!seconds) {
            return CommandResult::error("seconds must be 0.5 to 3600");
          }
        }
        const std::string text = game.recorder()->start(
            record::Recorder::Start{.folder = folder_arg(ctx, 1),
                                    .seconds = seconds},
            Game::records_classic());
        return lines(text, game.recorder()->recording());
      },
  });
  record.add_subcommand(CommandDef{
      .name = "stop",
      .description = "Stop recording; the files finish in the background",
      .handler = [&game](const CommandContext&) -> CommandResult {
        return lines(game.recorder()->stop());
      },
  });
  record.add_subcommand(CommandDef{
      .name = "status",
      .description = "How the recording goes, and what FFmpeg can do",
      .handler = [&game](const CommandContext&) -> CommandResult {
        static_cast<void>(game.recorder()->capabilities());
        return lines(game.recorder()->status());
      },
  });
  record.add_subcommand(CommandDef{
      .name = "clip",
      .description = "Batch mode: record the current screen for a while",
      .args = {ArgSpec{.name = "seconds",
                       .type = ArgType::String,
                       .description = "How long"},
               ArgSpec{.name = "folder",
                       .type = ArgType::String,
                       .optional = true,
                       .description = "Where the recording's folder goes"},
               ArgSpec{.name = "music",
                       .type = ArgType::Bool,
                       .optional = true,
                       .description = "Play an original song meanwhile"}},
      .handler = [&game, &state](const CommandContext& ctx) -> CommandResult {
        const auto seconds = seconds_of(ctx.string_arg(0));
        if (!seconds) {
          return CommandResult::error("seconds must be 0.5 to 3600");
        }
        const std::string text =
            game.record_clip(state, *seconds, folder_arg(ctx, 1),
                             ctx.has_arg(2) && ctx.bool_arg(2));
        if (text.empty()) {
          return CommandResult::error("Nothing to render");
        }
        return lines(text, game.recorder()->last_succeeded());
      },
  });
  return record;
}

} // namespace engine
