/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/PictureCommands.h"

#include <iostream>
#include <string>

#include "core/state/GameOptions.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "core/util/GameContext.h"
#include "engine/Game.h"
#include "engine/Screenshot.h"
#include "engine/fx/MapStore.h"
#include "engine/fx/PostFx.h"
#include "engine/fx/RendererSelect.h"

namespace fx {

using debug::command::ArgSpec;
using debug::command::ArgType;
using debug::command::CommandContext;
using debug::command::CommandDef;
using debug::command::CommandResult;

FrameContext frame_context(const GameState& state) {
  FrameContext c;
  const UIStateStack& ui = state.ui();
  if (const auto* r = ui.top_as<RestaurantViewState>()) {
    c.area = r->kitchen_view ? Area::Kitchen : Area::Restaurant;
  }
  else if (ui.is_top<PizzaDesignState>()) {
    c.area = Area::Kitchen;
  }
  else if (ui.is_top<CityViewState>()) {
    c.area = Area::City;
  }
  else if (const auto* tag = ui.top_as<ScreenTag>();
           tag && tag->kind == ScreenTag::Kind::MenuNorthPole) {
    c.area = Area::NorthPole;
  }
  c.hours = static_cast<float>(state.time().current_hour()) +
            static_cast<float>(state.time().current_minute()) / 60.0f;
  return c;
}

namespace {

// The settings, the renderer and what it can do, on one line.
std::string summary(const GameState& state) {
  const LookSettings& look = state.options().picture;
  std::string line = "picture:";
  for (std::string_view key : setting_keys()) {
    line += " " + std::string(key) + "=" + setting_value(look, key);
  }
  // The game's one window and its renderer.
  SDL_Renderer* r = nullptr;
  int count = 0;
  if (SDL_Window** windows = SDL_GetWindows(&count)) {
    if (count > 0) {
      r = SDL_GetRenderer(windows[0]);
    }
    SDL_free(windows);
  }
  const char* name = r ? SDL_GetRendererName(r) : nullptr;
  line += std::string(" | renderer=") + (name ? name : "none") +
          " post_fx_shader=" + (PostFx::is_gpu_driver(r) ? "yes" : "no") +
          " hdr=" + (r && renderer_hdr(r) ? "yes" : "no");
  const FrameState& f = frame_state();
  line += " | area=" + to_string(f.area) +
          " night=" + std::to_string(f.night).substr(0, 4) +
          " maps=" + (MapStore::instance().enabled() ? "yes" : "no");
  return line;
}

// Applies the settings to the frame at once (the game loop does it again
// each frame), so a bench or a capture in the next command sees them.
void apply_now(GameState& state) {
  set_frame_look(state.options().picture, frame_context(state));
}

CommandDef make_preset() {
  return CommandDef{
      .name = "preset",
      .description = "Choose the picture preset for this run",
      .help_text = R"(Usage: picture preset <name>

classic, natural, vivid, cinematic or studio. Classic is the HD look as it
always was.

Example:
  picture preset cinematic
)",
      .args = {ArgSpec{.name = "name",
                       .type = ArgType::String,
                       .optional = false,
                       .description = "The preset"}},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto& look = ctx.state.options().picture;
        if (auto failure =
                apply_setting(look, "picture_preset", ctx.string_arg(0))) {
          return CommandResult::error(*failure);
        }
        apply_now(ctx.state);
        return CommandResult::success(
            {"picture_preset is now " + setting_value(look, "picture_preset")});
      },
  };
}

CommandDef make_set() {
  std::string keys;
  for (std::string_view k : setting_keys()) {
    keys += "  " + std::string(k) + "\n";
  }
  return CommandDef{
      .name = "set",
      .description = "Change a picture setting for this run",
      .help_text = R"(Usage: picture set <key> <value>

The keys and values are spelled as settings.conf spells them:
)" + keys + R"(
Examples:
  picture set picture_bloom 30
  picture set weather_city snow
  picture set picture_day_night on
)",
      .args = {ArgSpec{.name = "key",
                       .type = ArgType::String,
                       .optional = false,
                       .description = "A picture_* or weather_* key"},
               ArgSpec{.name = "value",
                       .type = ArgType::String,
                       .optional = false,
                       .description = "Its new value"}},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto& look = ctx.state.options().picture;
        const std::string key = ctx.string_arg(0);
        if (auto failure = apply_setting(look, key, ctx.string_arg(1))) {
          return CommandResult::error(*failure);
        }
        apply_now(ctx.state);
        return CommandResult::success(
            {key + " is now " + setting_value(look, key)});
      },
  };
}

CommandDef make_shot(Game& game) {
  return CommandDef{
      .name = "shot",
      .description = "Write the screen through the picture pass to a PNG",
      .help_text = R"(Usage: picture shot [file]

Like `screenshot`, but the picture is the one the window shows: the preset,
its sliders, day and night, and the weather. On renderers other than SDL's
GPU driver that is the part fixed-function SDL can do (warmth, brightness,
lights, glow, snow). A bare file name goes to the screenshots folder.

Example:
  picture shot cinematic-city-night.png
)",
      .args = {ArgSpec{.name = "file",
                       .type = ArgType::String,
                       .optional = true,
                       .description = "Where to write the PNG"}},
      .handler = [&game](const CommandContext& ctx) -> CommandResult {
        const std::string name = ctx.has_arg(0) ? ctx.string_arg(0) : "";
        const std::string path = engine::resolve_screenshot_path(name);
        const FrameState saved = frame_state();
        FrameState graded = resolve_frame(ctx.state.options().picture,
                                          frame_context(ctx.state));
        graded.graded_capture = true;
        set_frame_state(graded);
        const bool ok = game.capture_screenshot(ctx.state, path);
        set_frame_state(saved);
        if (!ok) {
          return CommandResult::error("Nothing was written to " + path);
        }
        return CommandResult::success({"Wrote " + path});
      },
  };
}

} // namespace

std::vector<CommandDef> make_picture_commands(Game& game,
                                              GameContext& /*context*/,
                                              GameState& /*state*/) {
  std::vector<CommandDef> commands;
  commands.push_back(CommandDef{
      .name = "picture",
      .description = "Show or change the HD picture settings",
      .help_text = R"(Usage: picture [preset|set|shot]

Without a subcommand prints the picture settings and where the frame is
(area, night factor, whether the pack has maps) on one line, to the log and
to standard output, so review and bench runs can label what follows.
)",
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const std::string line = summary(ctx.state);
        std::cout << line << std::endl;
        return CommandResult::success({line});
      },
  }
                         .add_subcommand(make_preset())
                         .add_subcommand(make_set())
                         .add_subcommand(make_shot(game)));
  return commands;
}

} // namespace fx
