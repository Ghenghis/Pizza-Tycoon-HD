# Design note: `GameState::adopt_loaded(GameState&&)`

For the core lane, which owns `GameState`. It comes from the persistence lane (W01 follow-up) and
is not implemented. File references are to this checkout: `e55cb22`, plus the persistence lane's
working changes and the core lane's working `Roster` change to `GameState`.

## Why

`SaveGameManager::load` must leave the running game untouched when a save is turned away. Today it
gets there in two builds:

1. It builds the whole save into a scratch `GameState` (`SaveGameManager.cpp:1198`). Any damage
   shows up there.
2. It then builds the same document again into the live state, from the same random state
   (`SaveGameManager.cpp:1220`).

The second build exists only because the scratch state can't be moved into the live one from
outside `GameState`, for three reasons:

- The presenters are private `unique_ptr`s with no getter (`GameState.h:606-611`).
- `pit_accumulator_us_` has no accessor (`GameState.h:565`).
- Five sub-objects hold a pointer back to their owner, with no way to re-point it.

The two builds cost:

- Loading takes about twice as long, because every city is built from its map file twice.
- The second build can, in theory, fail after the first one passed. That leaves a half-built game
  and the message "the game in progress could not be kept".

`adopt_loaded` replaces the second build with a move. `load` would become:

```cpp
auto scratch = std::make_unique<GameState>();
scratch->options() = state.options();
json_to_state(*document, *scratch, context);   // throws: state untouched
state.adopt_loaded(std::move(*scratch));        // no build, cannot fail
state.finalize_initialization(context);
```

## What it does

It is `reset()` (`GameState.cpp:1199-1237`) with `loaded` in place of `GameState{}`. The members
`reset()` keeps across a new game, it also keeps. Everything else it takes from `loaded`. The
sub-objects keep the state the loader put in them and are pointed at `*this`, instead of being
rebuilt empty.

### Kept from `*this` (taken out before the move, put back after)

| Member | Line | Why |
|---|---|---|
| `staff_event_presenter_`, `monthly_entry_presenter_` | `GameState.h:606-607` | Registered once at startup by the UI. As `reset()` does. |
| `turn_presenter_`, `fade_presenter_` | `GameState.h:608, 610` | Same. |
| `tutorial_presenter_` | `GameState.h:611` | Same; a raw pointer. |
| `status_hub_` (the object) | `GameState.h:582` | `StatusDisplay` holds `StatusHub& hub_` (`StatusDisplay.h:38`) and subscribed to it (`StatusDisplay.cpp:36`). It is made from `gs.status()` (`Game.cpp:82, 344`). Keep the live object, then call `status_hub_.reset_for_new_game(time_)` (`StatusHub.cpp:40-46`), as `reset()` does. |
| `clock_rate_`, `pit_accumulator_us_` | `GameState.h:563, 565` | Not saved. As `reset()` does. |
| `turn_.requested_interruption_rate()` | `TurnOrchestrator.h:99-101` | Not saved. As `reset()` does. Restore it with `turn_.interruption_rate(rate)`. |
| `options_` | `GameState.h:583` | The player's settings, not the save's. As `reset()` does. Move it back rather than copy it, so nothing after the move can throw. |

### Kept from `*this` (proposed; `reset()` drops these today)

| Member | Line | Why |
|---|---|---|
| `window_width_`, `window_height_` | `GameState.h:560-561` | Display state. `reset()` zeroes them; a load shouldn't. |
| `pending_double_size_`, `pending_fullscreen_`, `pending_game_path_`, `pending_window_raise_`, `pending_crt_` | `GameState.h:592-596` | Requests the engine has not consumed yet. |
| `quit_` | `GameState.h:579` | An exit asked for before the load still stands. |
| `context_` | `GameState.h:603` | `finalize_initialization` sets it again straight after (`GameState.cpp:1084-1085`). Keeping it means nothing in between sees a null context. |

### Dropped

| Member | Line | Why |
|---|---|---|
| `no_turn_presenter_` | `GameState.h:609` | A `NoTurnPresenter` holds `GameState& gs_` (`TurnPresenter.h:68, 70, 114`). The scratch state makes one lazily when something asks for a presenter during the build (`GameState.cpp:59-67`), and that one refers to the scratch state. Reset the pointer to null; `turn_presenter()` makes a new one for `*this` when needed. It must never be moved across states. |

### Taken from `loaded`, then re-seated

These five hold `GameState* gs_` and keep their loaded contents: the deferred queues and day
flags, the competition count, and so on. The defaulted move assignment (`GameState.cpp:53`) copies
`gs_` from `loaded`, so each needs a one-line `rebind`:

```cpp
// The owner moved; point at the new one.
void rebind(GameState& state) { gs_ = &state; }
```

| Member | Line | `gs_` |
|---|---|---|
| `turn_` | `GameState.h:587` | `TurnOrchestrator.h:32, 261` |
| `competitions_` | `GameState.h:588` | `CompetitionCoordinator.h:34, 72` |
| `milestones_` | `GameState.h:589` | `MilestoneTracker.h:18, 43` |
| `real_estate_` | `GameState.h:590` | `RealEstate.h:25, 58` |
| `tutorial_` | `GameState.h:591` | `Tutorial.h:24, 48` |

`rebind` can be private, with `friend class GameState`, so nothing else re-points them.

### Taken from `loaded` as they are

These are values or heap-owned objects. Moving them keeps every address inside them valid:
`Property` holds `City&` to a heap `City`, and `Restaurant` holds `Property&` to a heap
`Property`.

| Lines (`GameState.h`) | Members |
|---|---|
| 536-553 | `time_`, `active_player_`, `active_city_`, `player_city_`, `price_update_category_`, `status_queue_city_`, `city_position_`, `active_district_index_`, `city_view_view_state_`, `roster_` (Classic for a loaded save; the comment at `GameState.h:550`, "Only finalize_initialization and reset assign it", then needs `adopt_loaded` added), `cities_`, `city_initialised_` |
| 557-559 | `players_`, `restaurants_`, `ui_` (fresh in `loaded`; `load` then calls `pop_to_root` and `replace_top`, as after `reset()` today) |
| 562 | `last_clicked_property_index_` |
| 564 | `clock_suspended_` (false, as after `reset()`) |
| 567-581 | `pit_tick_in_second_`, `second_in_block_`, `interruption_phase_`, `pending_screen_charge_blocks_`, `active_restaurant_`, `game_mode_`, `mission_target_`, `initializer_` (empty), `difficulty_level_`, `district_under_investigation_`, `time_scale_` (1, as after `reset()`), `next_restaurant_id_`, `furniture_placement_` |
| 584-586 | `rules_`, `starting_rules_`, `rules_changed_` |
| 597-601 | `article_database_` (empty; `finalize_initialization` loads it), `career_log_`, `customer_taste_profiles_`, `weapon_market_` |

## Sketch

```cpp
// Takes over a game the loader built apart from this one. Keeps what reset()
// keeps across a new game, and the engine's display requests; takes the rest
// from loaded and points its parts at this state. loaded is left empty.
void GameState::adopt_loaded(GameState&& loaded) {
  assert(&loaded != this);
  // A loaded game has no turn in progress; a pipeline step built for loaded
  // would not survive the move.
  assert(!loaded.turn_.is_processing_end_of_turn());

  auto staff_presenter = std::move(staff_event_presenter_);
  auto entry_presenter = std::move(monthly_entry_presenter_);
  auto turn_presenter = std::move(turn_presenter_);
  auto fade_presenter = std::move(fade_presenter_);
  auto* tutorial_presenter = tutorial_presenter_;
  auto hub = std::move(status_hub_);
  const ClockRate clock_rate = clock_rate_;
  const uint32_t accumulator = pit_accumulator_us_;
  const InterruptionRate rate = turn_.requested_interruption_rate();
  GameOptions options = std::move(options_);
  const int width = window_width_;
  const int height = window_height_;
  auto requests = std::tuple{pending_double_size_, pending_fullscreen_,
                             std::move(pending_game_path_),
                             pending_window_raise_, pending_crt_};
  const bool quit = quit_;
  GameContext* context = context_;

  *this = std::move(loaded);

  status_hub_ = std::move(hub);
  status_hub_.reset_for_new_game(time_);
  turn_.rebind(*this);
  competitions_.rebind(*this);
  milestones_.rebind(*this);
  real_estate_.rebind(*this);
  tutorial_.rebind(*this);
  no_turn_presenter_.reset();

  clock_rate_ = clock_rate;
  pit_accumulator_us_ = accumulator;
  turn_.interruption_rate(rate);
  options_ = std::move(options);
  window_width_ = width;
  window_height_ = height;
  std::tie(pending_double_size_, pending_fullscreen_, pending_game_path_,
           pending_window_raise_, pending_crt_) = std::move(requests);
  quit_ = quit;
  context_ = context;
  staff_event_presenter_ = std::move(staff_presenter);
  monthly_entry_presenter_ = std::move(entry_presenter);
  turn_presenter_ = std::move(turn_presenter);
  fade_presenter_ = std::move(fade_presenter);
  tutorial_presenter_ = tutorial_presenter;
}
```

Everything after the copies is a move, or a copy of a trivial type. If `std::function` and
`GameOptions` moves don't throw, as they don't on the three standard libraries we build with, the
function can be marked `noexcept`. Check `GameOptions` before adding it.

## Things to check while implementing

- **Pipeline.** `TurnOrchestrator`'s pipeline (`TurnOrchestrator.h:276-290`) holds `turn::Step`
  values; the steps are data, and `execute_pipeline_step` dispatches on them. A loaded state's
  pipeline is empty, which the assert above pins. If steps ever capture `this`, `adopt_loaded`
  must refuse a non-empty pipeline.
- **Presenters during the build.** The scratch build runs `active_city()` and with it
  `run_city_start_of_day` (`TurnOrchestrator.cpp:1472`). Any presenter call it makes reaches the
  scratch state's no-op presenters, not the live ones; the replay used to reach the live ones. I
  found none on that path (milestones go to the career log). Keep it that way, or have `load`
  repeat such calls after adopting.
- **Other back-references.** `ManagerRoutine` (`ManagerRoutine.h:44`), `StaffEvents`
  (`StaffEvents.h:68`) and the conversations (`BaseConversation.h:228`) hold a `GameState`, but
  they are built per call (`Restaurant.cpp:983, 1593`), not stored in the state.
- **Moved-from `loaded`.** It is destroyed right after. Its sub-objects still point at it, and its
  containers are empty; nothing should run on it.

## Tests to add with it (`tests/GameState*.cpp`)

1. **Fingerprints match.** Build a state, then adopt it into another. The fingerprint after
   adopting equals the scratch state's fingerprint before the move.
2. **Back-pointers re-seated.** After adopting, a deferred event restored through `turn()` is
   delivered by `*this`. `real_estate()` acts on `*this`'s cities. `turn_presenter()` with no
   presenter registered returns a `NoTurnPresenter` for `*this`.
3. **Status hub kept.** A subscriber on the live `status()` before adopting still gets messages
   posted after it.
4. **Settings kept.** Options, clock rate, interruption rate and window size survive.

Once it lands, the persistence lane switches `SaveGameManager::load` to it. That drops the replay,
the second build of every city, and the "could not be kept" message.
