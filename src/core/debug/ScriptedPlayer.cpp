/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/debug/ScriptedPlayer.h"

#include <utility>

#include "core/character/Player.h"
#include "core/state/CompetitionCoordinator.h"
#include "core/state/CompetitionSession.h"
#include "core/state/EndOfDayCause.h"
#include "core/state/GameState.h"
#include "core/state/TurnOrchestrator.h"
#include "core/state/UIState.h"
#include "core/time/GameDate.h"
#include "core/tutorial/Tutorial.h"

namespace debug::scripted {

void close_dialogs(GameState& state, const SeenFn& seen) {
  auto& ui = state.ui();
  for (int guard = 0;
       guard < 64 && ui.size() > 1 && !ui.is_top<CityViewState>(); ++guard) {
    std::function<void()> closed;
    if (auto* s = ui.top_as<FullscreenStatusState>()) {
      closed = s->on_dismiss;
    }
    else if (auto* s = ui.top_as<MaestroMessageState>()) {
      if (seen) {
        seen(s->text);
      }
      closed = s->on_dismiss;
    }
    else if (auto* s = ui.top_as<PlayerTurnCardState>()) {
      closed = s->on_closed;
    }
    else if (auto* s = ui.top_as<WeeksTakingsState>()) {
      closed = s->on_done;
    }
    else if (auto* s = ui.top_as<PizzaJudgesState>()) {
      closed = s->on_complete;
    }
    else if (auto* s = ui.top_as<PoliceState>()) {
      closed = s->on_complete;
    }
    else if (auto* s = ui.top_as<MayorState>()) {
      closed = s->on_complete;
    }
    else if (auto* s = ui.top_as<PizzaDesignState>()) {
      if (s->competition_session &&
          s->initial_mode == PizzaDesignMode::AnnualCompetitionDesign) {
        CompetitionSession session = *s->competition_session;
        ui.pop();
        state.competitions().finalize_annual_pizza_competition(
            std::move(session));
        continue;
      }
      closed = s->on_done;
    }
    else if (auto* s = ui.top_as<RestaurantStatsState>();
             s && s->automatic_mode) {
      // The end-of-day report holds the turn until its dismiss button,
      // which resumes the pipeline (RestaurantStats.cpp).
      ui.pop();
      state.turn().advance_pipeline();
      continue;
    }
    else if (ui.is_top<GameOverState>()) {
      return; // the game is over; the caller looks at why
    }
    ui.pop();
    if (closed) {
      closed();
    }
  }
}

bool human_turn(const GameState& state) {
  const auto* player = state.active_player();
  return player && player->is_human_controlled();
}

bool play_day(GameState& state, const SeenFn& seen) {
  const GameDate start = state.time().current_date();
  for (int tick = 0; tick < max_ticks_per_day; ++tick) {
    const bool new_day = state.time().current_date() != start;
    if (new_day && human_turn(state) &&
        !state.turn().is_pipeline_blocking_input()) {
      return true;
    }
    if (!new_day && human_turn(state) &&
        !state.turn().is_processing_end_of_turn()) {
      state.turn().request_end_of_turn(EndOfDayCause::PlayerAction);
    }
    state.update(simulation_tick_ms);
    state.tutorial().tick();
    close_dialogs(state, seen);
  }
  return false;
}

} // namespace debug::scripted
