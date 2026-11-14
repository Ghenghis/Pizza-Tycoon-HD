/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Golden fingerprints of an Expanded game: the first week in Santa's Pizza
// Village under the Cozy Village preset. It works the way the Classic golden
// suite (ClassicFingerprintTests.cpp) does: a fixed seed, a fixed script with
// no view (the console commands of a scripted run: rent, furnish piece by
// piece, menu, stock, staff, open), and SaveGameManager::fingerprint at a
// checkpoint after every day, compared with
// tests/fixtures/content/golden-fingerprints.json. The fingerprint hashes the
// whole save document except the time it was written, random state included,
// so a change to Expanded behaviour shows up as a changed value and has to be
// recorded on purpose.
//
// The values are recorded per platform. Nothing is ever recorded silently: a
// missing value fails. To record, run on the reference build with
// PLEGACY_UPDATE_GOLDEN=1, review the warnings and commit the file. The Classic
// golden file is a separate file and this test never reads or writes it.
// Needs the original game; SKIPs without PLEGACY_TEST_GAME_PATH.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "ContentJourney.h"
#include "ContentTestGame.h"
#include "core/character/Player.h"
#include "core/common/Ids.h"
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

// The Classic summary, plus what the person's pizzeria did, so a reviewer
// reading a changed value sees at once whether trading changed.
Observation observe(const content_test::TestGame& game,
                    const Restaurant& trading) {
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
  o.summary["pizzas_sold"] = trading.stats().total_pizzas_sold;
  o.summary["revenue_cents"] =
      static_cast<std::int64_t>(trading.stats().total_revenue);
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

TEST_CASE("Content golden: the first week in the village under Cozy Village",
          "[content][golden][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  Golden golden;
  content_test::TestGame game;
  // Seeded with content_journey::journey_seed; rents the cheapest premise,
  // furnishes it piece by piece, sets the menu, stocks, hires and opens.
  const content_journey::Journey j =
      content_journey::set_up_shop(game, content_journey::Fit::PieceByPiece);
  REQUIRE(j.trading != nullptr);
  const GameDate first_day = game.state.time().current_date();
  golden.check("first-week/started", observe(game, *j.trading));

  for (int day = 1; day <= 7; ++day) {
    content_test::play_day(game.state);
    REQUIRE(game.state.time().current_date() == first_day.add_days(day));
    golden.check("first-week/day-" + std::to_string(day),
                 observe(game, *j.trading));
  }
  // Not part of the golden value, only a sanity check on the script: the
  // pizzeria traded during the week.
  CHECK(j.trading->stats().total_pizzas_sold > 0);
  golden.finish();
}
