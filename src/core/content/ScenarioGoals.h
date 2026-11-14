/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>
#include <vector>

#include "core/content/Scenario.h"
#include "core/events/CareerLog.h"
#include "core/time/GameDate.h"

namespace core::content {

struct ObjectiveStatus {
  const Objective* objective{nullptr};
  // An achieve objective is met once its milestone is recorded (inside its
  // window, if it has one); an avoid objective while it is not.
  bool met{false};
  // When the milestone was recorded, if it was.
  std::optional<GameDate> recorded_on;
};

struct GoalProgress {
  std::vector<ObjectiveStatus> goal;
  std::vector<ObjectiveStatus> optional_objectives;
  // The day the last achieve objective of the goal was met with no avoid
  // objective of the goal broken by then. Once set it stays set: a CV entry
  // is never taken back.
  std::optional<GameDate> reached_on;
  // The goal can no longer be reached: an avoid objective was broken first,
  // or an achieve objective's window closed.
  bool failed{false};
};

// The goal observer's reading of one player's career log against a
// scenario's objectives (location-contract.md section 11.2). Pure: it reads
// recorded milestones only, so it adds nothing to the game state, the save
// or the random streams, and gives the same answer after a reload.
[[nodiscard]] GoalProgress
evaluate_goals(const Scenario& scenario,
               const std::vector<core::events::CareerLog::CvEntry>& cv,
               GameDate today);

// Whether any of milestones is an achieve objective of the goal: the goal can
// only have been completed in the delivery that showed one of them.
[[nodiscard]] bool
completes_goal(const Scenario& scenario,
               const std::vector<core::events::MilestoneId>& milestones);

} // namespace core::content
