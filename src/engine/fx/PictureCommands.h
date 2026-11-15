/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <vector>

#include "core/debug/command/CommandDef.h"
#include "engine/fx/Look.h"

class Game;
class GameContext;
class GameState;

namespace fx {

// What the game loop knows about the frame, for set_frame_look(): the area
// and time of day of the screen on top, the animation time from the game
// clock, the city's location and the city map's viewport.
[[nodiscard]] FrameContext frame_context(const GameState& state);

// The `picture` console commands (review captures and benches, see
// data/review/picture-presets.exec and bench-gpu.exec):
//   picture                     the settings and the renderer, one line
//   picture preset <name>       classic, natural, vivid, cinematic, studio
//   picture set <key> <value>   any picture_* or weather_* settings key
//   picture shot [file]         a screenshot through the picture pass
// Changes last for this run only, like `game options set`.
std::vector<debug::command::CommandDef>
make_picture_commands(Game& game, GameContext& context, GameState& state);

} // namespace fx
