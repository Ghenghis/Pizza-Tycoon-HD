/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/ScenarioGoals.h"

#include <algorithm>
#include <map>

namespace core::content {

namespace {

using core::events::MilestoneId;

bool inside_window(const Objective& o, const GameDate& when) {
  return !o.within_game_days ||
         when.days_since(game_start_date) <= *o.within_game_days;
}

bool window_closed(const Objective& o, const GameDate& today) {
  return o.within_game_days &&
         today.days_since(game_start_date) > *o.within_game_days;
}

} // namespace

GoalProgress
evaluate_goals(const Scenario& scenario,
               const std::vector<core::events::CareerLog::CvEntry>& cv,
               GameDate today) {
  std::map<MilestoneId, GameDate> first;
  for (const auto& entry : cv) {
    auto [it, inserted] = first.emplace(entry.message_id, entry.date);
    if (!inserted && entry.date < it->second) {
      it->second = entry.date;
    }
  }

  const auto status = [&](const Objective& o) {
    ObjectiveStatus s{.objective = &o, .met = false, .recorded_on = {}};
    if (const auto it = first.find(o.milestone); it != first.end()) {
      s.recorded_on = it->second;
    }
    const bool counted = s.recorded_on && inside_window(o, *s.recorded_on);
    s.met = o.polarity == Polarity::Achieve ? counted : !counted;
    return s;
  };

  GoalProgress progress;
  for (const auto& o : scenario.goal) {
    progress.goal.push_back(status(o));
  }
  for (const auto& o : scenario.optional_objectives) {
    progress.optional_objectives.push_back(status(o));
  }

  std::optional<GameDate> last_achieved;
  bool all_achieved = true;
  bool any_achieve = false;
  for (const auto& s : progress.goal) {
    if (s.objective->polarity != Polarity::Achieve) {
      continue;
    }
    any_achieve = true;
    if (!s.met) {
      all_achieved = false;
      if (window_closed(*s.objective, today)) {
        progress.failed = true;
      }
      continue;
    }
    if (!last_achieved || *last_achieved < *s.recorded_on) {
      last_achieved = s.recorded_on;
    }
  }

  // An avoid objective counts against the goal when its milestone came on
  // or before the day the goal would have been complete.
  for (const auto& s : progress.goal) {
    if (s.objective->polarity != Polarity::Avoid || s.met) {
      continue;
    }
    if (!all_achieved || !last_achieved || *s.recorded_on <= *last_achieved) {
      progress.failed = true;
    }
  }

  if (any_achieve && all_achieved && !progress.failed) {
    progress.reached_on = last_achieved;
  }
  return progress;
}

bool completes_goal(const Scenario& scenario,
                    const std::vector<core::events::MilestoneId>& milestones) {
  return std::any_of(scenario.goal.begin(), scenario.goal.end(),
                     [&](const Objective& o) {
                       return o.polarity == Polarity::Achieve &&
                              std::find(milestones.begin(), milestones.end(),
                                        o.milestone) != milestones.end();
                     });
}

} // namespace core::content
