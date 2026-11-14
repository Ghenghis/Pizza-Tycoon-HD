/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Golden fingerprints of Classic games (docs/studio/location-contract.md
// section 16, step 0). Each scenario starts from a fixed seed, plays a fixed
// script without any view (turns finish through NoTurnPresenter, as in a
// scripted run) and compares SaveGameManager::fingerprint at its checkpoints
// with tests/fixtures/classic/golden-fingerprints.json. The fingerprint hashes
// the whole save document except the time it was written, random state
// included, so an unchanged fingerprint means unchanged save bytes and an
// unchanged random sequence.
//
// The expected values are recorded per platform (floating point results can
// differ between compilers and C libraries). Nothing is ever recorded
// silently: a missing value fails. To record, run on a build whose Classic
// behaviour is the reference with PLEGACY_UPDATE_GOLDEN=1, then commit the
// file. These tests need the original game and SKIP without
// PLEGACY_TEST_GAME_PATH.
//
// Deliberately uses no Studio or roster code, so the same file records the
// reference on the code from before the roster refactor.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "TestSupport.h"
#include "core/GameTypes.h"
#include "core/common/Ids.h"
#include "core/init/NewGameConfig.h"
#include "core/save/SaveGameManager.h"
#include "core/state/CompetitionSession.h"
#include "core/state/EndOfDayCause.h"
#include "core/state/GameOptions.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "core/time/GameDate.h"
#include "core/time/GameTime.h"
#include "core/util/Constants.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/BobManager.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;
using save::SaveGameManager;
using test_support::TempDir;

namespace {

constexpr std::string_view golden_name = "golden-fingerprints.json";

// A day ends within a few hundred ticks; this only stops a stuck script.
constexpr int max_ticks_per_day = 200'000;

// Which compiler, system and processor produced a value.
std::string platform_id() {
  std::string os = "other";
#if defined(_WIN32)
  os = "windows";
#elif defined(__ANDROID__)
  os = "android";
#elif defined(__APPLE__)
  os = "macos";
#elif defined(__linux__)
  os = "linux";
#endif
  std::string compiler = "other";
#if defined(_MSC_VER) && !defined(__clang__)
  compiler = "msvc";
#elif defined(__clang__)
  compiler = "clang";
#elif defined(__GNUC__)
  compiler = "gcc";
#endif
  std::string arch = "other";
#if defined(_M_X64) || defined(__x86_64__)
  arch = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  arch = "arm64";
#endif
  return os + "-" + compiler + "-" + arch;
}

bool update_requested() {
  const char* env = std::getenv("PLEGACY_UPDATE_GOLDEN");
  return env && std::string_view(env) == "1";
}

// tests/fixtures/classic/golden-fingerprints.json, found from the source tree
// or the build folder.
fs::path golden_path() {
  const fs::path beside_source =
      fs::path(__FILE__).parent_path() / "fixtures" / "classic" / golden_name;
  if (fs::exists(beside_source)) {
    return beside_source;
  }
  for (const char* base :
       {"tests/fixtures/classic", "../tests/fixtures/classic",
        "../../tests/fixtures/classic"}) {
    if (fs::exists(fs::path(base) / golden_name)) {
      return fs::path(base) / golden_name;
    }
  }
  return beside_source;
}

std::string iso_date(const GameDate& date) {
  char text[16];
  std::snprintf(text, sizeof(text), "%04d-%02u-%02u", date.year(),
                static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()));
  return text;
}

// A Classic game run without views. The quick start loads standard pizzas,
// which needs the sprite sizes, so the context gets the sprite manager.
struct ClassicGame {
  BobManager bobs{BobLoader{}};
  GameContext context;
  GameState state;

  ClassicGame() { context.bob = &bobs; }
  ClassicGame(const ClassicGame&) = delete;
  ClassicGame& operator=(const ClassicGame&) = delete;
};

struct Start {
  std::uint32_t seed;
  GameMode mode;
  init::RestaurantSize size;
  init::DifficultyLevel difficulty;
  int city;
  bool fix_legacy_bugs = true;
};

// Starts a new game the way the new game menu does: player 1 is the only
// person, characters 0..7, fixed names.
void start_game(ClassicGame& game, const Start& start) {
  engine::random::seed(start.seed);
  auto& state = game.state;
  state.options().autosave_frequency = AutosaveFrequency::Off;
  state.options().rules.fix_legacy_bugs = start.fix_legacy_bugs;
  state.begin_initialization();
  auto& config = state.initializer();
  config.mode = start.mode;
  config.size = start.size;
  config.difficulty = start.difficulty;
  config.city_id = core::CityId{start.city};
  config.players.resize(NUMBER_OF_PLAYERS);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    config.players[i].name = "Player " + std::to_string(i + 1);
    config.players[i].character_id = i;
    config.players[i].is_human = (i == 0);
  }
  state.finalize_initialization(game.context);
  REQUIRE(state.active_player() != nullptr);
}

// Plays a person who closes every dialog at once: whatever is above the city
// view is popped and told it was closed, and the year's pizza competition
// gets no entry from them. Same choices on every run, so the script stays
// deterministic.
void close_dialogs(GameState& state) {
  auto& ui = state.ui();
  for (int guard = 0;
       guard < 64 && ui.size() > 1 && !ui.is_top<CityViewState>(); ++guard) {
    std::function<void()> closed;
    if (auto* s = ui.top_as<FullscreenStatusState>()) {
      closed = s->on_dismiss;
    }
    else if (auto* s = ui.top_as<MaestroMessageState>()) {
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
      // which resumes the pipeline (RestaurantStats.cpp:73-77). None of the
      // scripts below opens a restaurant, so none of them sees it.
      ui.pop();
      state.turn().advance_pipeline();
      continue;
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

// Ends the human player's day at once and runs the simulation until their
// next day has begun.
void play_day(GameState& state) {
  const GameDate start = state.time().current_date();
  for (int tick = 0; tick < max_ticks_per_day; ++tick) {
    const bool new_day = state.time().current_date() != start;
    if (new_day && human_turn(state) &&
        !state.turn().is_pipeline_blocking_input()) {
      return;
    }
    if (!new_day && human_turn(state) &&
        !state.turn().is_processing_end_of_turn()) {
      state.turn().request_end_of_turn(EndOfDayCause::PlayerAction);
    }
    state.update(simulation_tick_ms);
    state.tutorial().tick();
    close_dialogs(state);
  }
  FAIL("The day " << iso_date(start) << " did not end within "
                  << max_ticks_per_day << " ticks");
}

void play_days(GameState& state, int days) {
  for (int i = 0; i < days; ++i) {
    play_day(state);
  }
}

// Moves the calendar like `game date set`, keeping the time of day.
void jump_to(GameState& state, const GameDate& date) {
  auto time = state.time();
  const int time_of_day = time.time_blocks() % GameTime::BLOCKS_PER_DAY;
  time.time_blocks(
      (date.days_since(game_start_date) * GameTime::BLOCKS_PER_DAY) +
      time_of_day);
  state.set_time(time);
}

int restaurant_count(const GameState& state) {
  int count = 0;
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    count += static_cast<int>(state.restaurants_for(core::PlayerId{i}).size());
  }
  return count;
}

struct Observation {
  save::StateFingerprint fingerprint;
  json summary; // what is stored and compared
};

Observation observe(const ClassicGame& game) {
  Observation o{.fingerprint =
                    SaveGameManager::fingerprint(game.state, game.context),
                .summary = json::object()};
  o.summary["fingerprint"] = o.fingerprint.hash;
  o.summary["rng_state"] = engine::random::get_state();
  o.summary["date"] = iso_date(game.state.time().current_date());
  o.summary["time_blocks"] = game.state.time().time_blocks();
  o.summary["restaurants"] = restaurant_count(game.state);
  o.summary["cities_built"] =
      static_cast<int>(game.state.initialized_cities().size());
  return o;
}

// The expected values, read once per test and, when recording, written back.
class Golden {
public:
  Golden() : path_(golden_path()), platform_(platform_id()) {
    std::ifstream in(path_);
    REQUIRE(in.good());
    document_ = json::parse(in);
    REQUIRE(document_.contains("checkpoints"));
  }

  Golden(const Golden&) = delete;
  Golden& operator=(const Golden&) = delete;

  ~Golden() = default;

  // Writes what was recorded; called at the end of a test, so a test that
  // stops half way records nothing.
  void finish() {
    if (!recording_ || changed_ == 0) {
      return;
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out << document_.dump(2) << '\n';
    if (out.good()) {
      WARN("Recorded " << changed_ << " golden value(s) for " << platform_
                       << " in " << path_.string() << "; review and commit");
    }
    else {
      FAIL("Could not write " << path_.string());
    }
  }

  void check(const std::string& name, const Observation& actual) {
    INFO("checkpoint " << name << " on " << platform_);
    json& checkpoint = document_["checkpoints"][name];
    if (recording_) {
      if (checkpoint.is_object() && checkpoint.contains(platform_) &&
          checkpoint[platform_] != actual.summary) {
        WARN("Golden value changed for "
             << name << ": " << checkpoint[platform_].dump() << " -> "
             << actual.summary.dump());
      }
      if (!checkpoint.is_object()) {
        checkpoint = json::object();
      }
      checkpoint[platform_] = actual.summary;
      ++changed_;
      return;
    }
    if (!checkpoint.is_object() || !checkpoint.contains(platform_)) {
      FAIL_CHECK("No golden value recorded for "
                 << name << " on " << platform_
                 << ". Record it on the reference build with "
                    "PLEGACY_UPDATE_GOLDEN=1 and commit "
                 << path_.string());
      return;
    }
    const json& expected = checkpoint[platform_];
    if (expected != actual.summary) {
      const fs::path dump = fs::temp_directory_path() /
                            ("plegacy-golden-" + safe(name) + ".json");
      std::ofstream(dump) << actual.fingerprint.document.dump(1) << '\n';
      FAIL_CHECK("Classic state changed at "
                 << name << "\n expected " << expected.dump() << "\n actual   "
                 << actual.summary.dump() << "\n full document written to "
                 << dump.string());
    }
  }

private:
  static std::string safe(std::string name) {
    for (char& c : name) {
      if (c == '/' || c == '\\' || c == ':' || c == ' ') {
        c = '_';
      }
    }
    return name;
  }

  fs::path path_;
  std::string platform_;
  json document_;
  bool recording_ = update_requested();
  int changed_ = 0;
};

#define REQUIRE_GAME_DATA()                                                    \
  do {                                                                         \
    const auto game_path = test_support::game_path();                          \
    if (!game_path) {                                                          \
      SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");            \
    }                                                                          \
    fileutils::game_data_path(game_path->string());                            \
  } while (false)

} // namespace

TEST_CASE("Classic golden: a quick start in Madrid",
          "[classic][golden][game]") {
  REQUIRE_GAME_DATA();
  Golden golden;
  ClassicGame game;
  start_game(game, Start{.seed = 0x50495A5Au,
                         .mode = GameMode::QUICKSTART,
                         .size = init::RestaurantSize::BIG,
                         .difficulty = init::DifficultyLevel::PUSHOVER,
                         .city = 6});
  REQUIRE(restaurant_count(game.state) >= 1);
  golden.check("quick-start-madrid/started", observe(game));

  play_days(game.state, 3);
  golden.check("quick-start-madrid/day-4", observe(game));
  golden.finish();
}

TEST_CASE("Classic golden: a free game in Paris through turns, AI expansion, "
          "a month end and a year end",
          "[classic][golden][game]") {
  REQUIRE_GAME_DATA();
  Golden golden;
  ClassicGame game;
  start_game(game, Start{.seed = 1995u,
                         .mode = GameMode::FREEMODE,
                         .size = init::RestaurantSize::NONE,
                         .difficulty = init::DifficultyLevel::NORMAL,
                         .city = 0});
  const int restaurants_at_start = restaurant_count(game.state);
  golden.check("free-paris/started", observe(game));

  // Eight turn ends: a week end, and the AI's expansion cooldown runs out.
  play_days(game.state, 8);
  golden.check("free-paris/day-9", observe(game));
  INFO("restaurants at start " << restaurants_at_start);
  CHECK(restaurant_count(game.state) > restaurants_at_start); // AI expanded

  // Month end: the monthly competition, invoices and end-of-month processing.
  jump_to(game.state, GameDate{1995, 3, 30});
  play_days(game.state, 3);
  REQUIRE(game.state.time().current_date() == GameDate{1995, 4, 2});
  golden.check("free-paris/april", observe(game));

  // Year end: end-of-year processing and the annual pizza competition.
  jump_to(game.state, GameDate{1995, 12, 30});
  play_days(game.state, 3);
  REQUIRE(game.state.time().current_date() == GameDate{1996, 1, 2});
  const Observation new_year = observe(game);
  golden.check("free-paris/new-year", new_year);

  // A save of that game loads back to the same state.
  TempDir dir;
  const fs::path file = dir.path() / "classic-golden.plsave";
  REQUIRE(SaveGameManager::save(game.state, game.context, file.string()));
  ClassicGame loaded;
  std::string error;
  REQUIRE(SaveGameManager::load(loaded.state, loaded.context, file.string(),
                                &error));
  CHECK(error.empty());
  CHECK(SaveGameManager::fingerprint(loaded.state, loaded.context).hash ==
        new_year.fingerprint.hash);
  golden.finish();
}

TEST_CASE("Classic golden: all ten cities are built the same way",
          "[classic][golden][game]") {
  REQUIRE_GAME_DATA();
  Golden golden;
  for (const bool fix_legacy_bugs : {true, false}) {
    INFO("fix_legacy_bugs " << fix_legacy_bugs);
    ClassicGame game;
    start_game(game, Start{.seed = 0x0A11CC17u,
                           .mode = GameMode::FREEMODE,
                           .size = init::RestaurantSize::NONE,
                           .difficulty = init::DifficultyLevel::NORMAL,
                           .city = 0,
                           .fix_legacy_bugs = fix_legacy_bugs});
    for (int city = 1; city < NUMBER_OF_CITIES; ++city) {
      game.state.ensure_city_loaded(core::CityId{city});
    }
    REQUIRE(game.state.initialized_cities().size() ==
            static_cast<std::size_t>(NUMBER_OF_CITIES));
    golden.check(fix_legacy_bugs ? "ten-cities/built"
                                 : "ten-cities-original-bugs/built",
                 observe(game));
  }
  golden.finish();
}

TEST_CASE("Classic golden: the version 1 save fixture loads the same way",
          "[classic][golden][game][save]") {
  REQUIRE_GAME_DATA();
  Golden golden;
  const fs::path fixture =
      golden_path().parent_path().parent_path() / "saves" / "legacy_v1.plsave";
  REQUIRE(fs::exists(fixture));
  ClassicGame game;
  std::string error;
  REQUIRE(SaveGameManager::load(game.state, game.context, fixture.string(),
                                &error));
  CHECK(error.empty());
  golden.check("legacy-v1/loaded", observe(game));
  golden.finish();
}
