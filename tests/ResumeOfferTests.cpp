/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The start-up offer to resume: only after a run that did not end cleanly,
// only a verified timed checkpoint that run made after its last manual save,
// once, and never loading or deleting anything by itself.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "TestSupport.h"
#include "core/init/NewGameConfig.h"
#include "core/save/SaveGameManager.h"
#include "core/save/SaveVersions.h"
#include "core/save/Session.h"
#include "core/state/GameState.h"
#include "core/time/GameTime.h"
#include "core/util/GameContext.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using save::ResumeCandidate;
using save::SaveGameManager;
using save::SaveSlot;
using save::SaveVersion;
using save::SessionRecord;
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

SaveVersion timed(std::uint64_t sequence, std::string written_at,
                  VersionStatus status = VersionStatus::Working) {
  return SaveVersion{
      .sequence = sequence,
      .status = status,
      .level = status == VersionStatus::Working ? VersionLevel::LoadVerified
                                                : VersionLevel::IntegrityChecked,
      .reason = status == VersionStatus::Working ? "" : "it loads as another game",
      .made_for = "checkpoint after five minutes",
      .written_at = std::move(written_at),
      .size = 1,
      .checksum = save::checksum_of("x"),
      .fingerprint = "00000000000000aa",
      .game_date = "Sat Mar  4 1995"};
}

SlotManifest manifest_of(std::initializer_list<SaveVersion> versions) {
  SlotManifest m;
  m.versions = versions;
  std::uint64_t newest = 0;
  for (const SaveVersion& v : m.versions) {
    newest = std::max(newest, v.sequence);
    if (v.status == VersionStatus::Working &&
        (!m.active || v.sequence > *m.active)) {
      m.active = v.sequence;
    }
  }
  m.next_sequence = newest + 1;
  return m;
}

SessionRecord crashed(std::string started_at, std::string manual = "") {
  return {.started_at = std::move(started_at),
          .clean_exit = false,
          .last_manual_save_at = std::move(manual)};
}

// Ends the session a test began, so no other test notes saves into it.
struct SessionGuard {
  ~SessionGuard() { SaveGameManager::end_session(); }
};

} // namespace

TEST_CASE("Resuming is offered only after a run that did not end cleanly",
          "[save][resume]") {
  const SlotManifest checkpoints =
      manifest_of({timed(3, "2026-11-14T10:20:00Z"),
                   timed(2, "2026-11-14T10:15:00Z"),
                   timed(1, "2026-11-13T22:00:00Z")});

  SECTION("no record of the last run: nothing") {
    CHECK_FALSE(save::choose_resume(std::nullopt, checkpoints));
  }
  SECTION("a clean exit: nothing") {
    SessionRecord clean = crashed("2026-11-14T10:00:00Z");
    clean.clean_exit = true;
    CHECK_FALSE(save::choose_resume(clean, checkpoints));
  }
  SECTION("no timed checkpoints: nothing") {
    CHECK_FALSE(save::choose_resume(crashed("2026-11-14T10:00:00Z"),
                                    std::nullopt));
  }
  SECTION("a crash: the newest checkpoint that run made") {
    const auto offer =
        save::choose_resume(crashed("2026-11-14T10:00:00Z"), checkpoints);
    REQUIRE(offer);
    CHECK(offer->sequence == 3);
    CHECK(offer->written_at == "2026-11-14T10:20:00Z");
    CHECK(offer->game_date == "Sat Mar  4 1995");
  }
  SECTION("checkpoints from before that run: nothing") {
    CHECK_FALSE(save::choose_resume(crashed("2026-11-14T11:00:00Z"),
                                    checkpoints));
  }
  SECTION("a manual save after the last checkpoint: nothing") {
    CHECK_FALSE(save::choose_resume(
        crashed("2026-11-14T10:00:00Z", "2026-11-14T10:21:00Z"),
        checkpoints));
  }
  SECTION("a manual save between checkpoints: the later one") {
    const auto offer = save::choose_resume(
        crashed("2026-11-14T10:00:00Z", "2026-11-14T10:17:00Z"), checkpoints);
    REQUIRE(offer);
    CHECK(offer->sequence == 3);
  }
  SECTION("a checkpoint that did not pass its checks is never offered") {
    const SlotManifest failed =
        manifest_of({timed(4, "2026-11-14T10:30:00Z", VersionStatus::Candidate),
                     timed(3, "2026-11-14T10:20:00Z")});
    const auto offer =
        save::choose_resume(crashed("2026-11-14T10:00:00Z"), failed);
    REQUIRE(offer);
    CHECK(offer->sequence == 3);
  }
}

TEST_CASE("The session record reads back as written, and nothing else reads",
          "[save][resume]") {
  const SessionRecord record = crashed("2026-11-14T10:00:00Z",
                                       "2026-11-14T10:05:00Z");
  const SessionRecord back =
      save::session_from_json(save::session_to_json(record));
  CHECK(back.started_at == record.started_at);
  CHECK(back.clean_exit == record.clean_exit);
  CHECK(back.last_manual_save_at == record.last_manual_save_at);

  for (const char* text :
       {R"({})", R"({"format":"plegacy-session","format_version":2})",
        R"({"format":"plegacy-session","format_version":1,"started_at":"x",)"
        R"("last_manual_save_at":""})",
        R"([1])"}) {
    INFO(text);
    CHECK_THROWS(save::session_from_json(nlohmann::json::parse(text)));
  }
}

TEST_CASE("A session records crashes and clean exits, and deletes nothing",
          "[save][resume]") {
  TempDir dir;
  SessionGuard guard;
  const fs::path record_file = dir.path() / save::session_filename;
  const SaveSlot slot(dir.path() / save::timed_checkpoint_filename);
  fs::create_directories(slot.folder());
  write_file(slot.version_path(1), "x");
  write_file(slot.file(), "x");

  // The first run: nothing to offer, and it is recorded as running.
  SaveGameManager::begin_session(dir.path());
  CHECK_FALSE(SaveGameManager::pending_resume());
  std::string problem;
  auto record = save::read_session(record_file, problem);
  REQUIRE(record);
  CHECK_FALSE(record->clean_exit);

  // It made a checkpoint, then the game was killed.
  REQUIRE(slot.write_manifest(manifest_of({timed(1, "2999-01-01T00:00:00Z")})));
  const std::string manifest_text = read_file(slot.manifest_path());

  SaveGameManager::begin_session(dir.path());
  const auto pending = SaveGameManager::pending_resume();
  REQUIRE(pending);
  CHECK(pending->sequence == 1);
  // Starting up wrote nothing but the record.
  CHECK(read_file(slot.manifest_path()) == manifest_text);
  CHECK(read_file(slot.version_path(1)) == "x");

  // That version is not a real save, so it fails its check before any build
  // and nothing is offered; asking again gives nothing either.
  GameState state;
  GameContext context;
  CHECK_FALSE(SaveGameManager::take_resume_offer(state, context));
  CHECK_FALSE(SaveGameManager::pending_resume());
  CHECK(read_file(slot.version_path(1)) == "x");

  // A clean exit: the next start is quiet.
  SaveGameManager::end_session();
  record = save::read_session(record_file, problem);
  REQUIRE(record);
  CHECK(record->clean_exit);
  SaveGameManager::begin_session(dir.path());
  CHECK_FALSE(SaveGameManager::pending_resume());
}

TEST_CASE("After a crash, the last timed checkpoint resumes the game",
          "[save][resume][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());
  SaveGameManager::finish_background_saves();

  TempDir dir;
  SessionGuard guard;
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

  SaveGameManager::begin_session(dir.path());
  const fs::path timed_slot = dir.path() / save::timed_checkpoint_filename;
  auto job = SaveGameManager::start_timed_checkpoint(
      state, context, save::TimedCheckpoint::Interval, nullptr, timed_slot);
  REQUIRE(job);
  job->wait();
  REQUIRE(job->ready());
  REQUIRE(SaveGameManager::promote(*job) == save::CheckpointResult::Written);
  const std::string kept = SaveGameManager::fingerprint(state, context).hash;
  const int blocks = state.time().time_blocks();

  // The game is killed: no end_session. The next start offers it.
  SaveGameManager::begin_session(dir.path());
  GameState menu;
  const auto offer = SaveGameManager::take_resume_offer(menu, context);
  REQUIRE(offer);
  CHECK(offer->sequence == job->sequence());
  CHECK(offer->day == blocks / GameTime::BLOCKS_PER_DAY + 1);
  CHECK(offer->time.size() == 5);
  CHECK_FALSE(SaveGameManager::take_resume_offer(menu, context));

  // Resume: through load_version, to the very game.
  std::string error;
  REQUIRE(SaveGameManager::load_version(menu, context, offer->slot_file,
                                        offer->sequence, &error, nullptr,
                                        dir.path() / save::checkpoint_filename));
  CHECK(SaveGameManager::fingerprint(menu, context).hash == kept);

  // A manual save after the checkpoint: after the next crash, nothing.
  REQUIRE(SaveGameManager::save_to_slot(
      menu, context, (dir.path() / "mine.plsave").string()));
  SaveGameManager::begin_session(dir.path());
  CHECK_FALSE(SaveGameManager::pending_resume());
}
