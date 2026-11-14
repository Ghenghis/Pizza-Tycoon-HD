/*
 * SPDX-FileCopyrightText: 2012 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/debug/DebugConsole.h"
#include "core/state/GameState.h"
#include "core/util/GameContext.h"

// Registers every console command that needs no engine objects. A refused
// command is logged and kept in the registry's registration_errors().
void register_core_commands(debug::command::CommandRegistry& registry);

void register_debug_commands(DebugConsole& console, GameState& state,
                             GameContext& context);
