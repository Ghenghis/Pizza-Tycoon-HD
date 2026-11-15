/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Golden fingerprints of an Expanded game: the North Pole slice, Santa's
// Pizza Village under the Cozy Village preset through its first week. The
// run is data/review/north-pole-slice.exec, the very script the Windows and
// Linux builds run with --exec: this test reads it and runs each line through
// the console in-process, the same commands in the same order (new game with
// a fixed seed, rent, furnish piece by piece, menu, stock, staff, open, seven
// days, save, load). At each `dump fingerprint` it takes
// SaveGameManager::fingerprint, under the name of the "#@checkpoint" line
// before it, and compares it with
// tests/fixtures/content/golden-fingerprints.json, the way the Classic golden
// suite (ClassicFingerprintTests.cpp) does. The fingerprint hashes the whole
// save document except the time it was written, random state included, so a
// change to Expanded behaviour shows up as a changed value and has to be
// recorded on purpose.
//
// The values are recorded per platform. Nothing is ever recorded silently: a
// missing value fails. To record, run on the reference build with
// PLEGACY_UPDATE_GOLDEN=1, review the warnings and commit the file. The Classic
// golden file is a separate file and this test never reads or writes it.
//
// Hidden ([.]): it needs the original game (PLEGACY_TEST_GAME_PATH) and the
// game's data folder next to the test binary (run it from the build folder),
// and `game scenario` keeps a checkpoint in the saves folder as it does in
// the game. Run it by name or tag: test_plegacy "[slice]". The script's save
// and load go to a temporary folder instead of the saves folder, and its
// screenshots are skipped (there is no screen here); they come after the
// last fingerprint.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ContentTestGame.h"
#include "TestSupport.h"
#include "core/character/Player.h"
#include "core/common/Ids.h"
#include "core/content/ShippedScenarios.h"
#include "core/debug/DebugCommands.h"
#include "core/debug/command/CommandDef.h"
#include "core/debug/command/CommandRegistry.h"
#include "core/debug/command/CommandResult.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantStats.h"
#include "core/save/SaveGameManager.h"
#include "core/state/GameState.h"
#include "core/time/GameDate.h"
#include "core/time/GameTime.h"
#include "core/util/Constants.h"
#include "engine/Random.h"

namespace fs = std::filesystem;
using nlohmann::json;
using save::SaveGameManager;

namespace {

constexpr std::string_view golden_name = "golden-fingerprints.json";
constexpr std::string_view golden_format = "pthd.content-golden";

// Which compiler, system and processor produced a value: the same names as
// the Classic golden file.
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

// tests/fixtures/content/golden-fingerprints.json, found from the source tree
// or the build folder.
fs::path golden_path() {
  const fs::path beside_source =
      fs::path(__FILE__).parent_path() / "fixtures" / "content" / golden_name;
  if (fs::exists(beside_source)) {
    return beside_source;
  }
  for (const char* base :
       {"tests/fixtures/content", "../tests/fixtures/content",
        "../../tests/fixtures/content"}) {
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

// The Classic summary, plus what the person's pizzerias did, so a reviewer
// reading a changed value sees at once whether trading changed.
Observation observe(const content_test::TestGame& game) {
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
  int sold = 0;
  std::int64_t revenue = 0;
  for (const Restaurant* r : game.state.restaurants_for(core::PlayerId{0})) {
    sold += r->stats().total_pizzas_sold;
    revenue += static_cast<std::int64_t>(r->stats().total_revenue);
  }
  o.summary["pizzas_sold"] = sold;
  o.summary["revenue_cents"] = revenue;
  o.summary["cash_cents"] = static_cast<std::int64_t>(
      game.state.player(core::PlayerId{0})->cash_balance());
  return o;
}

// The expected values, read once per test and, when recording, written back.
class Golden {
public:
  Golden() : path_(golden_path()), platform_(platform_id()) {
    std::ifstream in(path_);
    INFO(path_.string());
    REQUIRE(in.good());
    document_ = json::parse(in);
    REQUIRE(document_.value("format", std::string{}) == golden_format);
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
                            ("plegacy-content-golden-" + safe(name) + ".json");
      std::ofstream(dump) << actual.fingerprint.document.dump(1) << '\n';
      FAIL_CHECK("Expanded state changed at "
                 << name << "\n expected " << expected.dump() << "\n actual   "
                 << actual.summary.dump() << "\n full document written to "
                 << dump.string()
                 << "\n If the change is meant, record it with "
                    "PLEGACY_UPDATE_GOLDEN=1 and commit the file.");
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

} // namespace

namespace {

fs::path slice_script() {
  return fs::path(__FILE__).parent_path().parent_path() / "data" / "review" /
         "north-pole-slice.exec";
}

// One line of an --exec script as Game::run_exec_script reads it: blanks
// stripped; empty and '#' lines are not commands.
std::string trimmed(std::string line) {
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
    line.pop_back();
  }
  const std::size_t start = line.find_first_not_of(" \t");
  return start == std::string::npos ? std::string{} : line.substr(start);
}

} // namespace

TEST_CASE("Content golden: the North Pole slice script, its first week in "
          "the village",
          "[.][content][golden][slice][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  if (!fs::exists(core::content::shipped_data_dir() / "scenarios" /
                  "north-pole-cozy-village.json")) {
    SKIP("The game's data folder ("
         << core::content::shipped_data_dir().string()
         << ") has no scenarios; run from the build folder");
  }
  std::ifstream script(slice_script());
  REQUIRE(script.good());

  // The console of the game, without the screen: `screenshot` is the one
  // command the script uses that needs a screen, and it does nothing here.
  debug::command::CommandRegistry console;
  register_core_commands(console);
  console.register_command(debug::command::CommandDef{
      .name = "screenshot",
      .description = "Skipped: no screen in this test",
      .args = {debug::command::ArgSpec{
          .name = "file",
          .type = debug::command::ArgType::String,
          .optional = true,
          .default_provider = debug::command::DefaultProvider::None,
          .description = "Ignored",
      }},
      .handler =
          [](const debug::command::CommandContext&) {
            return debug::command::CommandResult::success();
          },
  });

  test_support::TempDir saves;
  Golden golden;
  content_test::TestGame game;
  std::string checkpoint;
  std::vector<std::string> taken;
  std::optional<std::string> day_7;
  std::optional<std::string> reloaded;
  std::string line;
  while (std::getline(script, line)) {
    line = trimmed(line);
    if (line.empty()) {
      continue;
    }
    if (line.starts_with("#@checkpoint ")) {
      checkpoint = trimmed(line.substr(13));
      continue;
    }
    if (line[0] == '#') {
      continue;
    }
    // The save and the load go to a temporary folder (an absolute name
    // replaces the saves folder).
    for (const std::string_view verb : {"game save ", "game load "}) {
      if (line.starts_with(verb)) {
        line = std::string(verb) +
               (saves.path() / line.substr(verb.size())).string();
      }
    }
    INFO("script line: " << line);
    const auto result = console.execute(line, game.state, game.context);
    INFO((result.error_message ? *result.error_message : std::string{}));
    REQUIRE(result.status == debug::command::CommandStatus::Success);

    if (line == "dump fingerprint") {
      REQUIRE_FALSE(checkpoint.empty());
      const Observation o = observe(game);
      golden.check(checkpoint, o);
      taken.push_back(checkpoint);
      if (checkpoint == "first-week/day-7") {
        day_7 = o.fingerprint.hash;
      }
      if (checkpoint == "first-week/reloaded") {
        reloaded = o.fingerprint.hash;
      }
      checkpoint.clear();
    }
  }

  // Every checkpoint of the week, in order, and nothing else.
  const std::vector<std::string> expected = {
      "first-week/started", "first-week/day-1", "first-week/day-2",
      "first-week/day-3",   "first-week/day-4", "first-week/day-5",
      "first-week/day-6",   "first-week/day-7", "first-week/reloaded"};
  CHECK(taken == expected);
  // The save loads back to the game it was.
  REQUIRE(day_7.has_value());
  REQUIRE(reloaded.has_value());
  CHECK(*reloaded == *day_7);
  // The pizzerias traded during the week (not part of the golden value).
  int sold = 0;
  for (const Restaurant* r : game.state.restaurants_for(core::PlayerId{0})) {
    sold += r->stats().total_pizzas_sold;
  }
  CHECK(sold > 0);
  // The mid-week restocks leave money in the till after the week's wages.
  CHECK(game.state.player(core::PlayerId{0})->cash_balance() > Money{0});
  golden.finish();
}

namespace {

// The slice script from a new game through day 7 with another seed: the
// lines up to the save (the save, the load and the pictures add nothing to
// the week). after(line) runs after each line that succeeded.
void play_slice_week(content_test::TestGame& game,
                     debug::command::CommandRegistry& console,
                     std::uint32_t seed,
                     const std::function<void(const std::string&)>& after) {
  std::ifstream script(slice_script());
  REQUIRE(script.good());
  const std::string start = "game scenario north-pole-cozy-village ";
  bool started = false;
  std::string line;
  while (std::getline(script, line)) {
    line = trimmed(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.starts_with("game save ")) {
      break;
    }
    if (line.starts_with(start)) {
      line = start + std::to_string(seed);
      started = true;
    }
    INFO("seed " << seed << ", script line: " << line);
    const auto result = console.execute(line, game.state, game.context);
    INFO((result.error_message ? *result.error_message : std::string{}));
    REQUIRE(result.status == debug::command::CommandStatus::Success);
    after(line);
  }
  REQUIRE(started);
}

} // namespace

TEST_CASE("The North Pole slice trades whatever the seed",
          "[content][slice][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  if (!fs::exists(core::content::shipped_data_dir() / "scenarios" /
                  "north-pole-cozy-village.json")) {
    SKIP("The game's data folder ("
         << core::content::shipped_data_dir().string()
         << ") has no scenarios; run from the build folder");
  }
  debug::command::CommandRegistry console;
  register_core_commands(console);

  // The seeds the slice was tried with; 23162 is the script's own. On it a
  // cook blew up the only oven in the first version of the script, and the
  // cheese gets destroyed on day 7 (both staff events of the original).
  for (const std::uint32_t seed : {1u, 7u, 1224u, 23162u}) {
    INFO("seed " << seed);
    content_test::TestGame game;
    std::optional<core::RestaurantId> shop;
    int days = 0;
    play_slice_week(game, console, seed, [&](const std::string& line) {
      if (line.starts_with("city rent ")) {
        REQUIRE(game.state.active_restaurant() != nullptr);
        shop = game.state.active_restaurant()->id();
      }
      if (line.starts_with("game day")) {
        ++days;
        REQUIRE(shop.has_value());
        const Restaurant* r = game.state.restaurant(*shop);
        REQUIRE(r != nullptr);
        INFO("day " << days);
        // Never without a working oven.
        CHECK(r->calculate_daily_oven_throughput() > 0);
      }
    });
    CHECK(days == 7);
    REQUIRE(shop.has_value());
    const Restaurant* r = game.state.restaurant(*shop);
    REQUIRE(r != nullptr);
    CHECK(r->stats().total_pizzas_sold > 0);
    CHECK(r->calculate_daily_oven_throughput() > 0);
    // Still solvent on day 7, after the week's wages.
    CHECK(game.state.player(core::PlayerId{0})->cash_balance() >= Money{0});
  }
}
