/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/CheckpointTimer.h"

#include "core/time/GameTime.h"

namespace save {

void CheckpointTimer::played(std::chrono::milliseconds time) {
  if (time.count() > 0) {
    played_ += time;
  }
}

void CheckpointTimer::observe(const GameTime& time) {
  const int day = time.time_blocks() / GameTime::BLOCKS_PER_DAY;
  if (day_ && day != *day_) {
    // A week begins when the day before it ended one. A new week outranks
    // a new day still waiting.
    const TimedCheckpoint now =
        day_ends_week_ ? TimedCheckpoint::NewWeek : TimedCheckpoint::NewDay;
    if (boundary_ != TimedCheckpoint::NewWeek) {
      boundary_ = now;
    }
  }
  day_ = day;
  day_ends_week_ = time.current_date().is_end_of_week();
}

std::optional<TimedCheckpoint> CheckpointTimer::due() const {
  if (boundary_) {
    return boundary_;
  }
  if (played_ >= CHECKPOINT_INTERVAL) {
    return TimedCheckpoint::Interval;
  }
  return std::nullopt;
}

void CheckpointTimer::taken() {
  played_ = std::chrono::milliseconds{0};
  boundary_.reset();
}

} // namespace save
