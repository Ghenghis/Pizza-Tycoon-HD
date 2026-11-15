/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The Recovery Center's model: the save folder as rows, paged and chosen,
// and the actions on them. Browsing reads files only, so the game in
// progress and the folder stay exactly as they were.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/init/NewGameConfig.h"
#include "core/save/RecoveryBrowser.h"
#include "core/save/SaveGameManager.h"
#include "core/save/SaveVersions.h"
#include "core/state/GameState.h"
#include "core/time/GameTime.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using save::RecoveryAction;
using save::RecoveryBrowser;
using save::RecoveryRow;
using save::SaveGameManager;
using save::SaveSlot;
using save::SaveVersion;
using save::SlotManifest;
using save::VersionLevel;
using save::VersionStatus;
using test_support::TempDir;

namespace {

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

// Every file under the folder with its bytes, to show nothing changed.
std::map<std::string, std::string> snapshot(const fs::path& folder) {
  std::map<std::string, std::string> files;
  for (const auto& entry : fs::recursive_directory_iterator(folder)) {
    if (entry.is_regular_file()) {
      files[fs::relative(entry.path(), folder).generic_string()] =
          read_file(entry.path());
    }
  }
  return files;
}

SaveVersion version(std::uint64_t sequence, VersionStatus status,
                    VersionLevel level, std::string reason,
                    std::string made_for = "save") {
  return SaveVersion{.sequence = sequence,
                     .status = status,
                     .level = level,
                     .reason = std::move(reason),
                     .made_for = std::move(made_for),
                     .written_at = "2026-11-14T09:30:12Z",
                     .size = 1,
                     .checksum = save::checksum_of("x"),
                     .fingerprint = "00000000000000aa",
                     .game_date = "Sat Mar  4 1995"};
}

SaveVersion working(std::uint64_t sequence, std::string made_for = "save") {
  return version(sequence, VersionStatus::Working, VersionLevel::LoadVerified,
                 "", std::move(made_for));
}

void write_slot(const fs::path& folder, const std::string& name,
                SlotManifest manifest) {
  const SaveSlot slot(folder / name);
  fs::create_directories(slot.folder());
  write_file(slot.file(), "x");
  for (const SaveVersion& v : manifest.versions) {
    write_file(slot.version_path(v.sequence), "x");
  }
  REQUIRE(slot.write_manifest(manifest));
}

// A folder with every kind of slot the list shows.
void fill_folder(const fs::path& folder) {
  SlotManifest checkpoints;
  checkpoints.next_sequence = 4;
  checkpoints.active = 2;
  checkpoints.versions = {
      version(3, VersionStatus::Candidate, VersionLevel::IntegrityChecked,
              "it loads as another game", "checkpoint before loading a save"),
      working(2, "checkpoint before a new game"),
  };
  write_slot(folder, "CHECKPNT.plsave", checkpoints);

  SlotManifest game;
  game.next_sequence = 3;
  game.active = 2;
  game.previous_active = 1;
  game.versions = {working(2), working(1)};
  write_slot(folder, "game.plsave", game);

  write_file(folder / "a.plsave", "one plain file");

  const SaveSlot lost(folder / "lost.plsave");
  fs::create_directories(lost.folder());
  write_file(lost.manifest_path(), "{ damaged");
}

const RecoveryRow& row_of(const std::vector<RecoveryRow>& rows,
                          std::string_view slot,
                          std::optional<std::uint64_t> sequence = {}) {
  const auto it = std::ranges::find_if(rows, [&](const RecoveryRow& r) {
    return r.slot == slot && r.sequence == sequence;
  });
  REQUIRE(it != rows.end());
  return *it;
}

bool contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// A game in progress built without the original game's files.
struct LiveGame {
  core::character::CharacterData character{};
  GameState state;
  GameContext context;

  LiveGame() {
    std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
    for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
      players[i] = std::make_unique<Player>(
          core::PlayerId{i}, core::character::Character(&character), i == 0);
    }
    state.players(std::move(players));
    state.active_player(core::PlayerId{0});
    engine::random::set_state(0x5EED1234);
  }
};

} // namespace

TEST_CASE("The save folder becomes rows: checkpoints first, then each save "
          "and its versions",
          "[save][recovery]") {
  TempDir dir;
  fill_folder(dir.path());
  const auto rows =
      save::recovery_rows(SaveGameManager::recovery_slots(dir.path()));

  // The order of the list.
  std::vector<std::pair<std::string, std::optional<std::uint64_t>>> order;
  for (const RecoveryRow& r : rows) {
    order.emplace_back(r.slot, r.sequence);
  }
  using Key = std::pair<std::string, std::optional<std::uint64_t>>;
  CHECK(order == std::vector<Key>{{"CHECKPNT.plsave", std::nullopt},
                                  {"CHECKPNT.plsave", 3},
                                  {"CHECKPNT.plsave", 2},
                                  {"a.plsave", std::nullopt},
                                  {"game.plsave", std::nullopt},
                                  {"game.plsave", 2},
                                  {"game.plsave", 1},
                                  {"lost.plsave", std::nullopt}});

  const RecoveryRow& checkpoints = row_of(rows, "CHECKPNT.plsave");
  CHECK(checkpoints.checkpoint);
  CHECK(contains(checkpoints.text, "made by the game"));
  CHECK(contains(checkpoints.text, "1 working, 1 other"));

  // A working version: its level, time, game day and why it was made.
  const RecoveryRow& kept = row_of(rows, "CHECKPNT.plsave", 2);
  CHECK(kept.kind == RecoveryRow::Kind::Version);
  CHECK(kept.active);
  CHECK_FALSE(kept.damaged);
  for (const char* part : {"#2", "load-verified", "2026-11-14 09:30",
                           "Sat Mar  4 1995", "before new game", "*"}) {
    INFO(part);
    CHECK(contains(kept.text, part));
  }
  CHECK(kept.can_load);
  CHECK(kept.can_branch);
  CHECK_FALSE(kept.can_undo);

  // A version that failed a check says so, and why.
  const RecoveryRow& failed = row_of(rows, "CHECKPNT.plsave", 3);
  CHECK(failed.damaged);
  CHECK(contains(failed.text, "damaged (integrity-checked)"));
  CHECK(contains(failed.detail, "it loads as another game"));
  CHECK_FALSE(failed.can_load);
  CHECK_FALSE(failed.can_branch);

  // A save with a restore to undo.
  const RecoveryRow& game = row_of(rows, "game.plsave");
  CHECK(game.can_undo);
  CHECK(contains(game.detail, "version 1"));
  CHECK_FALSE(row_of(rows, "game.plsave", 1).active);

  // A single file loads as it is; a damaged list of versions is reported.
  CHECK(row_of(rows, "a.plsave").can_load);
  CHECK(contains(row_of(rows, "a.plsave").text, "no versions"));
  const RecoveryRow& lost = row_of(rows, "lost.plsave");
  CHECK(lost.damaged);
  CHECK(contains(lost.text, "damaged"));
  CHECK_FALSE(lost.detail.empty());
  CHECK_FALSE(lost.can_load); // it has no file of its own
}

TEST_CASE("A branch gets a name no save has", "[save][recovery]") {
  TempDir dir;
  fill_folder(dir.path());
  auto slots = SaveGameManager::recovery_slots(dir.path());
  CHECK(save::branch_name("game.plsave", 2, slots) == "game-2.plsave");
  write_file(dir.path() / "GAME-2.plsave", "taken, in another case");
  slots = SaveGameManager::recovery_slots(dir.path());
  CHECK(save::branch_name("game.plsave", 2, slots) == "game-2b.plsave");
}

TEST_CASE("The browser pages, chooses and offers only what can be done",
          "[save][recovery]") {
  TempDir dir;
  fill_folder(dir.path());
  RecoveryBrowser browser(3, dir.path());
  CHECK(browser.rows().size() == 8);
  CHECK(browser.page_count() == 3);
  CHECK(browser.summary() == "Page 1 of 3: 4 saves, 4 versions");
  CHECK_FALSE(browser.chosen());
  for (const RecoveryAction action :
       {RecoveryAction::Load, RecoveryAction::Branch, RecoveryAction::Undo}) {
    CHECK_FALSE(browser.can(action));
  }

  browser.previous_page();
  CHECK(browser.page() == 0);
  browser.next_page();
  browser.next_page();
  browser.next_page();
  CHECK(browser.page() == 2);
  CHECK(browser.row_on_page(0)->slot == "game.plsave");
  CHECK(browser.row_on_page(1)->slot == "lost.plsave");
  CHECK(browser.row_on_page(2) == nullptr);

  browser.choose(0); // game.plsave, version 1
  REQUIRE(browser.chosen());
  CHECK(browser.chosen()->sequence == std::optional<std::uint64_t>{1});
  CHECK(browser.is_chosen(0));
  CHECK(browser.can(RecoveryAction::Load));
  CHECK(browser.can(RecoveryAction::Branch));
  CHECK_FALSE(browser.can(RecoveryAction::Undo));
  CHECK(contains(browser.confirmation(RecoveryAction::Load),
                 "Load version 1 of game?"));
  CHECK(contains(browser.confirmation(RecoveryAction::Branch), "game-1"));

  // Choosing past the end of a page does nothing.
  browser.choose(2);
  CHECK(browser.chosen()->sequence == std::optional<std::uint64_t>{1});

  // Reading the folder again keeps the choice, and its page.
  browser.previous_page();
  browser.refresh();
  CHECK(browser.page() == 2);
  CHECK(browser.chosen()->sequence == std::optional<std::uint64_t>{1});

  // An action that can't be done does nothing.
  browser.previous_page();
  LiveGame game;
  browser.choose(0); // the first row of page 2: a.plsave
  REQUIRE(browser.chosen());
  CHECK(browser.chosen()->slot == "a.plsave");
  CHECK_FALSE(browser.perform(RecoveryAction::Branch, game.state, game.context)
                  .done);
  CHECK_FALSE(
      browser.perform(RecoveryAction::Undo, game.state, game.context).done);
}

TEST_CASE("Browsing changes neither the game nor the save folder",
          "[save][recovery]") {
  TempDir dir;
  fill_folder(dir.path());
  LiveGame game;
  const std::string before =
      SaveGameManager::fingerprint(game.state, game.context).hash;
  const std::uint32_t stream = engine::random::get_state();
  const auto files = snapshot(dir.path());

  // Everything the view does while the player looks.
  RecoveryBrowser browser(2, dir.path());
  for (std::size_t page = 0; page < browser.page_count(); ++page) {
    for (std::size_t i = 0; i < browser.rows_per_page(); ++i) {
      browser.choose(i);
      for (const RecoveryAction action : {RecoveryAction::Load,
                                          RecoveryAction::Branch,
                                          RecoveryAction::Undo}) {
        static_cast<void>(browser.can(action));
        static_cast<void>(browser.confirmation(action));
      }
      if (const RecoveryRow* row = browser.chosen()) {
        static_cast<void>(row->detail);
      }
    }
    static_cast<void>(browser.summary());
    browser.next_page();
  }
  browser.refresh();

  CHECK(SaveGameManager::fingerprint(game.state, game.context).hash == before);
  CHECK(engine::random::get_state() == stream);
  CHECK(snapshot(dir.path()) == files);
}

TEST_CASE("Restore as a new save and Load this version, on real saves",
          "[save][recovery][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());
  SaveGameManager::finish_background_saves();

  TempDir dir;
  GameContext context;
  GameState state;
  state.begin_initialization();
  auto& config = state.initializer();
  config.mode = GameMode::FREEMODE;
  config.difficulty = init::DifficultyLevel::NORMAL;
  config.city_id = core::CityId{0};
  config.players.resize(NUMBER_OF_PLAYERS);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    config.players[i].name = "Player " + std::to_string(i + 1);
    config.players[i].character_id = i;
    config.players[i].is_human = (i == 0);
  }
  state.finalize_initialization(context);

  const fs::path file = dir.path() / "game.plsave";
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string first =
      SaveGameManager::fingerprint(state, context).hash;
  GameTime later = state.time();
  later.time_blocks(later.time_blocks() + 6);
  state.set_time(later);
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string manifest_before = read_file(SaveSlot(file).manifest_path());

  RecoveryBrowser browser(20, dir.path());
  const auto choose = [&browser](std::string_view slot,
                                 std::optional<std::uint64_t> sequence) {
    for (std::size_t i = 0; i < browser.rows_per_page(); ++i) {
      const RecoveryRow* row = browser.row_on_page(i);
      if (row && row->slot == slot && row->sequence == sequence) {
        browser.choose(i);
        return;
      }
    }
    FAIL("no such row");
  };

  choose("game.plsave", 1);
  const auto branched =
      browser.perform(RecoveryAction::Branch, state, context);
  INFO(branched.message);
  REQUIRE(branched.done);
  CHECK(fs::exists(dir.path() / "game-1.plsave"));
  CHECK(read_file(SaveSlot(file).manifest_path()) == manifest_before);
  // The new save shows up, with its one version.
  CHECK(std::ranges::any_of(browser.rows(), [](const RecoveryRow& r) {
    return r.slot == "game-1.plsave" && r.sequence == 1;
  }));

  // Loading version 1 gives that game, keeping this one as a checkpoint in
  // the same folder.
  choose("game.plsave", 1);
  const auto loaded = browser.perform(RecoveryAction::Load, state, context);
  INFO(loaded.message);
  REQUIRE(loaded.done);
  CHECK(SaveGameManager::fingerprint(state, context).hash == first);
  CHECK(fs::exists(dir.path() / save::checkpoint_filename));
  CHECK(read_file(SaveSlot(file).manifest_path()) == manifest_before);
}
