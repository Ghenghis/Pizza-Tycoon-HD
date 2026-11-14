/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// When the game keeps a timed checkpoint: after every five minutes of play,
// and when a game day or week begins. Only the timing; the checkpoint itself
// is SaveGameManager's, and it writes nothing when the game is unchanged.

#include <chrono>
#include <optional>

class GameTime;

namespace save {

// Play time between two interval checkpoints. Time in menus, paused or with
// the window in the background does not count.
constexpr std::chrono::minutes CHECKPOINT_INTERVAL{5};

enum class TimedCheckpoint {
  Interval, // five minutes of play since the last one
  NewDay,   // a game day began
  NewWeek,  // a game week began
};

class CheckpointTimer {
public:
  // Adds frame time the game was being played.
  void played(std::chrono::milliseconds time);

  // The game's time, every frame. The first call only notes the day.
  void observe(const GameTime& time);

  // The checkpoint due now, if any. A new week counts over a new day, and a
  // boundary over the interval.
  [[nodiscard]] std::optional<TimedCheckpoint> due() const;

  // Whether a game day or week began since the last checkpoint.
  [[nodiscard]] bool boundary_pending() const { return boundary_.has_value(); }

  // A checkpoint was taken, or found unneeded: start counting again.
  void taken();

private:
  std::chrono::milliseconds played_{0};
  std::optional<int> day_;     // the game day last seen
  bool day_ends_week_ = false; // whether that day ends a week
  std::optional<TimedCheckpoint> boundary_;
};

} // namespace save
