/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// A scripted person for runs without a screen (console scripts, --exec,
// tests): one who ends each day at once and closes every dialog the moment it
// appears, the same way on every run, so a script plays the same game every
// time. `game dismiss` and `game day` are this player.

#include <functional>
#include <string>

class GameState;

namespace debug::scripted {

// Called with the text of each Maestro message as it is closed.
using SeenFn = std::function<void(const std::string&)>;

// Closes whatever is above the city view: each dialog is popped and told it
// was closed, the year's pizza competition gets no entry, and the end-of-day
// report is dismissed (which resumes the turn). Stops at a game over, which
// it leaves for the caller.
void close_dialogs(GameState& state, const SeenFn& seen = nullptr);

// Whether the active player is a person.
[[nodiscard]] bool human_turn(const GameState& state);

// The most ticks a day may take before play_day gives up.
inline constexpr int max_ticks_per_day = 200'000;

// Ends the person's day at once and runs the simulation, closing dialogs,
// until their next day has begun. False if it did not within
// max_ticks_per_day ticks (a game over, for one, ends no day).
[[nodiscard]] bool play_day(GameState& state, const SeenFn& seen = nullptr);

} // namespace debug::scripted
