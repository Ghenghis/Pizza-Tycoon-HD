/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/debug/command/CommandDef.h"

class Game;
class GameState;

namespace engine {

// record start|stop|status|clip: the game's own picture and sound to
// Matroska segments (docs/studio/notes/recording.md).
debug::command::CommandDef make_record_command(Game& game, GameState& state);

} // namespace engine
