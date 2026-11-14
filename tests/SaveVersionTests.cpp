/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A save slot keeps its last three verified working versions and the newest
// candidate. A new save never costs a working version until it has passed
// its checks, and every failure on the way, on real files, leaves the slot
// loading what it loaded before.
//
// The faults come from util::testing::ScopedAtomicFault, which makes chosen
// AtomicFile writes fail or lose bytes the way a disk does, and from damaging
// the files directly. The save code itself runs unchanged.
//
// A version is only load-verified by building it, which needs the original
// game's files. The cases that get that far are tagged [game] and SKIP
// without PLEGACY_TEST_GAME_PATH; the rest stop before any build.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/init/NewGameConfig.h"
#include "core/save/SaveGameManager.h"
#include "core/save/SaveVersions.h"
#include "core/state/GameState.h"
#include "core/time/GameTime.h"
#include "core/util/AtomicFile.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;
using save::SaveGameManager;
using save::SaveSlot;
using save::SaveVersion;
using save::SlotManifest;
using save::VersionLevel;
using save::VersionStatus;
using test_support::TempDir;
using util::testing::AtomicFault;
using util::testing::ScopedAtomicFault;

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

std::set<std::string> names_in(const fs::path& dir) {
  std::set<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    names.insert(entry.path().filename().string());
  }
  return names;
}

// Matches the AtomicFile writes to a slot's version files.
ScopedAtomicFault::Matcher version_files() {
  return [](const fs::path& destination) {
    return save::version_sequence(destination.filename().string()).has_value();
  };
}

ScopedAtomicFault::Matcher named(std::string name) {
  return [name = std::move(name)](const fs::path& destination) {
    return destination.filename().string() == name;
  };
}

SaveVersion working_version(std::uint64_t sequence, std::string_view bytes,
                            std::string fingerprint) {
  const save::FileDigest digest = save::digest_of(bytes);
  return SaveVersion{.sequence = sequence,
                     .status = VersionStatus::Working,
                     .level = VersionLevel::LoadVerified,
                     .reason = "",
                     .made_for = "save",
                     .written_at = "2026-11-14T09:30:00Z",
                     .size = digest.size,
                     .checksum = digest.checksum,
                     .fingerprint = std::move(fingerprint),
                     .game_date = ""};
}

SaveVersion candidate_version(std::uint64_t sequence) {
  SaveVersion v = working_version(sequence, "x", "0123456789abcdef");
  v.status = VersionStatus::Candidate;
  v.level = VersionLevel::IntegrityChecked;
  v.reason = "it loads as another game";
  return v;
}

std::vector<std::uint64_t> sequences(const SlotManifest& manifest) {
  std::vector<std::uint64_t> out;
  for (const SaveVersion& v : manifest.versions) {
    out.push_back(v.sequence);
  }
  return out;
}

// A game in progress built without the original game's files: all eight
// players, the first a person, sharing a hand-made character.
struct LiveGame {
  core::character::CharacterData character{};
  GameState state;
  GameContext context;

  LiveGame() {
    std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
    for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
      players[i] = std::make_unique<Player>(
          core::PlayerId{i}, core::character::Character(&character), i == 0);
      players[i]->name("Player " + std::to_string(i + 1));
    }
    state.players(std::move(players));
    state.active_player(core::PlayerId{0});
    GameTime time;
    time.time_blocks(9 * 6 + 40);
    state.set_time(time);
    engine::random::set_state(0x5EED1234);
  }
  LiveGame(const LiveGame&) = delete;
  LiveGame& operator=(const LiveGame&) = delete;
};

// A slot with one working version of the game, made by hand the way a save
// leaves it: the version file, the slot's file and the manifest.
struct PreparedSlot {
  fs::path file;
  std::string bytes;
  std::string manifest_text;
};

PreparedSlot prepare_slot(const TempDir& dir, LiveGame& game,
                          const std::string& name = "game.plsave") {
  PreparedSlot slot{
      .file = dir.path() / name, .bytes = {}, .manifest_text = {}};
  REQUIRE(SaveGameManager::save(game.state, game.context, slot.file.string()));
  slot.bytes = read_file(slot.file);
  const SaveSlot versions(slot.file);
  fs::create_directories(versions.folder());
  write_file(versions.version_path(1), slot.bytes);
  SlotManifest manifest;
  manifest.next_sequence = 2;
  manifest.active = 1;
  manifest.versions = {working_version(
      1, slot.bytes,
      SaveGameManager::fingerprint(game.state, game.context).hash)};
  REQUIRE(versions.write_manifest(manifest));
  slot.manifest_text = read_file(versions.manifest_path());
  return slot;
}

// The slot as prepare_slot left it, whatever was tried since: the working
// version, its file, the slot's file, and no temporary file anywhere.
void require_untouched(const PreparedSlot& slot) {
  const SaveSlot versions(slot.file);
  CHECK(read_file(slot.file) == slot.bytes);
  CHECK(read_file(versions.version_path(1)) == slot.bytes);
  const auto read = versions.read_manifest();
  REQUIRE(read.manifest);
  CHECK(read.manifest->active == std::optional<std::uint64_t>{1});
  const SaveVersion* working = read.manifest->newest_working();
  REQUIRE(working != nullptr);
  CHECK(working->sequence == 1);
  for (const auto& folder : {slot.file.parent_path(), versions.folder()}) {
    for (const auto& name : names_in(folder)) {
      INFO(name);
      CHECK_FALSE(util::is_temp_file_name(name));
    }
  }
}

// A new free game in city 0 with eight players, which needs the original
// game's files.
void start_free_game(GameState& state, GameContext& context) {
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
}

// Another game: the same one an hour on.
void move_on(GameState& state) {
  GameTime time = state.time();
  time.time_blocks(time.time_blocks() + 6);
  state.set_time(time);
}

std::string hash_of(const GameState& state, const GameContext& context) {
  return SaveGameManager::fingerprint(state, context).hash;
}

// Loads the slot into a fresh game and gives that game's fingerprint.
std::string load_hash(const fs::path& file, GameContext& context,
                      std::string* notice = nullptr) {
  GameState loaded;
  std::string error;
  REQUIRE(
      SaveGameManager::load(loaded, context, file.string(), &error, notice));
  CHECK(error.empty());
  return hash_of(loaded, context);
}

#define REQUIRE_GAME_DATA()                                                    \
  do {                                                                         \
    const auto game_path = test_support::game_path();                          \
    if (!game_path) {                                                          \
      SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");            \
    }                                                                          \
    fileutils::game_data_path(game_path->string());                            \
  } while (false)

} // namespace

// The file side

TEST_CASE("Version files have one name each", "[save][versions]") {
  CHECK(save::version_sequence("v000013.plsave") == 13);
  CHECK(save::version_sequence("v1234567.plsave") == 1234567);
  for (const char* name :
       {"v13.plsave", "v0000013.plsave", "v000000.plsave", "v00001a.plsave",
        "x000013.plsave", "v000013.plsave.tmp-0123456789ab", "manifest.json"}) {
    INFO(name);
    CHECK_FALSE(save::version_sequence(name));
  }
  const SaveSlot slot(fs::path("saves") / "GAME.plsave");
  CHECK(slot.folder() == fs::path("saves") / "GAME.plsave.versions");
  CHECK(slot.version_path(13) == slot.folder() / "v000013.plsave");
  CHECK(save::is_versions_folder_name("GAME.plsave.versions"));
  CHECK_FALSE(save::is_versions_folder_name(".plsave.versions"));
  // CRC-32 of "123456789", the standard check value.
  CHECK(save::checksum_of("123456789") == "crc32:cbf43926");
}

TEST_CASE("A manifest reads back as written, and nothing else reads",
          "[save][versions]") {
  SlotManifest manifest;
  manifest.next_sequence = 8;
  manifest.active = 7;
  manifest.versions = {candidate_version(5),
                       working_version(7, "seven", "00000000000000aa"),
                       working_version(6, "six", "00000000000000bb")};
  manifest.retired = {2};
  const json written = save::manifest_to_json(manifest, "GAME.plsave");
  const SlotManifest read = save::manifest_from_json(written);
  CHECK(sequences(read) == std::vector<std::uint64_t>{7, 6, 5}); // newest first
  CHECK(read.active == std::optional<std::uint64_t>{7});
  CHECK(read.retired == std::vector<std::uint64_t>{2});
  CHECK(read.find(5)->reason == "it loads as another game");
  CHECK(read.find(6)->checksum == save::checksum_of("six"));

  const auto with = [&written](const auto& change) {
    json j = written;
    change(j);
    return j;
  };
  for (const json& bad : {
           with([](json& j) { j["format"] = "something else"; }),
           with([](json& j) { j["versions"][0]["sequence"] = 8; }),
           with([](json& j) { j["versions"][0]["sequence"] = 7; }),
           with([](json& j) {
             j["versions"][1]["level"] = "integrity-checked";
           }),
           with([](json& j) { j["versions"][0]["checksum"] = "md5:00"; }),
           with([](json& j) { j["versions"][0]["fingerprint"] = "short"; }),
           with([](json& j) { j["versions"][0]["size"] = -1; }),
           with([](json& j) { j["active"] = 5; }),
           with([](json& j) { j["retired"] = {6}; }),
           with([](json& j) { j.erase("next_sequence"); }),
           json::array(),
       }) {
    INFO(bad.dump());
    CHECK_THROWS(save::manifest_from_json(bad));
  }
}

TEST_CASE("A slot keeps three working versions and the newest candidate",
          "[save][versions]") {
  SlotManifest manifest;
  manifest.next_sequence = 10;
  for (std::uint64_t s = 1; s <= 5; ++s) {
    manifest.versions.push_back(working_version(s, "x", "00000000000000aa"));
  }
  manifest.active = 5;

  SECTION("working versions past three go") {
    save::apply_retention(manifest);
    CHECK(sequences(manifest) == std::vector<std::uint64_t>{5, 4, 3});
    CHECK(manifest.retired == std::vector<std::uint64_t>{2, 1});
  }
  SECTION("a failed save costs no working version") {
    manifest.versions.push_back(candidate_version(6));
    save::apply_retention(manifest);
    CHECK(sequences(manifest) == std::vector<std::uint64_t>{6, 5, 4, 3});
    // A second failure replaces the first candidate.
    manifest.versions.push_back(candidate_version(7));
    save::apply_retention(manifest);
    CHECK(sequences(manifest) == std::vector<std::uint64_t>{7, 5, 4, 3});
  }
  SECTION("a candidate goes once a newer save works") {
    manifest.versions.push_back(candidate_version(6));
    manifest.versions.push_back(working_version(7, "x", "00000000000000aa"));
    manifest.active = 7;
    save::apply_retention(manifest);
    CHECK(sequences(manifest) == std::vector<std::uint64_t>{7, 5, 4});
  }
  SECTION("the active version stays, however old") {
    manifest.active = 1;
    save::apply_retention(manifest);
    CHECK(sequences(manifest) == std::vector<std::uint64_t>{5, 4, 3, 1});
  }
}

TEST_CASE("A damaged or newer manifest is reported and changes nothing",
          "[save][versions]") {
  TempDir dir;
  const SaveSlot slot(dir.path() / "game.plsave");
  CHECK_FALSE(slot.read_manifest().manifest);
  CHECK(slot.read_manifest().problem.empty());

  fs::create_directories(slot.folder());
  write_file(slot.version_path(3), "kept");
  for (const char* text : {"{", "[]", R"({"format":"plegacy-save-versions"})",
                           "[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]"}) {
    INFO(text);
    write_file(slot.manifest_path(), text);
    const auto read = slot.read_manifest();
    CHECK_FALSE(read.manifest);
    CHECK_FALSE(read.problem.empty());
    CHECK_FALSE(read.newer);
  }

  json newer = save::manifest_to_json(SlotManifest{}, "game.plsave");
  newer["format_version"] = save::SAVE_VERSIONS_FORMAT + 1;
  write_file(slot.manifest_path(), newer.dump());
  CHECK(slot.read_manifest().newer);

  CHECK(slot.version_files() == std::vector<std::uint64_t>{3});
  CHECK(read_file(slot.version_path(3)) == "kept");
}

// AtomicFile's faults, on real files

TEST_CASE("AtomicFile faults leave the old file as a full disk or a crash "
          "would",
          "[save][atomic][fault]") {
  TempDir dir;
  const fs::path file = dir.path() / "save.plsave";
  write_file(file, "the good save\n");
  const std::string fresh(4096, 'n');

  SECTION("the disk fills up partway") {
    ScopedAtomicFault fault(
        named("save.plsave"),
        {.kind = AtomicFault::Kind::DiskFull, .bytes = 100});
    const util::AtomicWriteResult result =
        util::write_file_atomically(file, fresh);
    CHECK_FALSE(result);
    CHECK(result.code == std::errc::no_space_on_device);
    CHECK(fault.hits() == 1);
    CHECK(read_file(file) == "the good save\n");
    CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
  }
  SECTION("the rename fails") {
    ScopedAtomicFault fault(named("save.plsave"),
                            {.kind = AtomicFault::Kind::RenameFails});
    CHECK_FALSE(util::write_file_atomically(file, fresh));
    CHECK(read_file(file) == "the good save\n");
    CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
  }
  SECTION("the disk loses the end of the write") {
    ScopedAtomicFault fault(named("save.plsave"),
                            {.kind = AtomicFault::Kind::LostTail, .bytes = 10});
    // AtomicFile can't tell; the reader has to.
    CHECK(util::write_file_atomically(file, fresh));
    CHECK(read_file(file) == fresh.substr(0, 10));
  }
  SECTION("the game is killed before the rename") {
    ScopedAtomicFault fault(named("save.plsave"),
                            {.kind = AtomicFault::Kind::Interrupted});
    CHECK_THROWS_AS(util::write_file_atomically(file, fresh),
                    util::testing::AtomicFileInterrupted);
    CHECK(read_file(file) == "the good save\n");
    // The half-done write is left as a crash leaves it, for the startup
    // cleanup to find.
    const auto names = names_in(dir.path());
    CHECK(names.size() == 2);
    CHECK(std::ranges::count_if(names, [](const std::string& name) {
            return util::is_temp_file_name(name);
          }) == 1);
  }
  SECTION("other files are not touched") {
    ScopedAtomicFault fault(named("other.plsave"),
                            {.kind = AtomicFault::Kind::RenameFails});
    CHECK(util::write_file_atomically(file, fresh));
    CHECK(fault.hits() == 0);
  }
}

// Saving to a slot: failures before any build

TEST_CASE("A slot save that fails on the way costs no working version",
          "[save][versions][fault]") {
  TempDir dir;
  LiveGame game;
  const PreparedSlot slot = prepare_slot(dir, game);
  const SaveSlot versions(slot.file);
  std::string error;

  SECTION("the disk fills up writing the new version") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::DiskFull, .bytes = 64});
    CHECK_FALSE(SaveGameManager::save_to_slot(game.state, game.context,
                                              slot.file.string(), &error));
    CHECK(fault.hits() == 1);
    CHECK(error.find("disk is full") != std::string::npos);
    // The failed version is listed as a candidate with the reason.
    const auto read = versions.read_manifest();
    REQUIRE(read.manifest);
    const SaveVersion* failed = read.manifest->find(2);
    REQUIRE(failed != nullptr);
    CHECK(failed->status == VersionStatus::Candidate);
    CHECK(failed->reason.starts_with("it could not be written"));
  }
  SECTION("the disk loses the end of the new version") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::LostTail, .bytes = 40});
    CHECK_FALSE(SaveGameManager::save_to_slot(game.state, game.context,
                                              slot.file.string(), &error));
    CHECK(error.find("did not pass its checks") != std::string::npos);
    // It was written, but the integrity check caught it before any build.
    const auto read = versions.read_manifest();
    REQUIRE(read.manifest);
    const SaveVersion* failed = read.manifest->find(2);
    REQUIRE(failed != nullptr);
    CHECK(failed->status == VersionStatus::Candidate);
    CHECK(failed->level == VersionLevel::Written);
    CHECK(failed->reason.find("bytes on disk, not the") != std::string::npos);
  }
  SECTION("the rename of the new version fails") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::RenameFails});
    CHECK_FALSE(SaveGameManager::save_to_slot(game.state, game.context,
                                              slot.file.string(), &error));
    CHECK(error.starts_with("The game could not be saved"));
    CHECK_FALSE(fs::exists(versions.version_path(2)));
  }
  SECTION("the manifest can't be written") {
    ScopedAtomicFault fault(named("manifest.json"),
                            {.kind = AtomicFault::Kind::RenameFails});
    CHECK_FALSE(SaveGameManager::save_to_slot(game.state, game.context,
                                              slot.file.string(), &error));
    CHECK(read_file(versions.manifest_path()) == slot.manifest_text);
    CHECK_FALSE(fs::exists(versions.version_path(2)));
  }
  SECTION("the game is killed writing the new version") {
    {
      ScopedAtomicFault fault(version_files(),
                              {.kind = AtomicFault::Kind::Interrupted});
      CHECK_THROWS_AS(SaveGameManager::save_to_slot(game.state, game.context,
                                                    slot.file.string()),
                      util::testing::AtomicFileInterrupted);
    }
    // The manifest already accounts for it, as a candidate being written.
    const auto read = versions.read_manifest();
    REQUIRE(read.manifest);
    REQUIRE(read.manifest->find(2) != nullptr);
    CHECK(read.manifest->find(2)->reason == "being written");
    // The startup cleanup looks in the versions folder too.
    for (const auto& name : names_in(versions.folder())) {
      if (util::is_temp_file_name(name)) {
        fs::last_write_time(versions.folder() / name,
                            fs::file_time_type::clock::now() -
                                std::chrono::hours(2));
      }
    }
    CHECK(SaveGameManager::remove_stale_temp_files(
              {versions.folder()}, save::STALE_TEMP_FILE_AGE) == 1);
  }
  require_untouched(slot);
}

TEST_CASE("A slot whose every copy is damaged is turned away whole",
          "[save][versions][load]") {
  TempDir dir;
  LiveGame game;
  const PreparedSlot slot = prepare_slot(dir, game);
  const SaveSlot versions(slot.file);
  std::string damaged = slot.bytes;
  damaged[damaged.size() / 2] ^= 0x40;
  write_file(slot.file, damaged);
  write_file(versions.version_path(1), damaged);

  const std::string before = hash_of(game.state, game.context);
  const std::size_t ui_depth = game.state.ui().size();
  std::string error;
  std::string notice;
  CHECK_FALSE(SaveGameManager::load(game.state, game.context,
                                    slot.file.string(), &error, &notice));
  CHECK(error == "This save file is damaged and cannot be loaded.");
  CHECK(notice.empty());
  CHECK(hash_of(game.state, game.context) == before);
  CHECK(game.state.ui().size() == ui_depth);
}

TEST_CASE("No checkpoint without a game, and none when nothing changed",
          "[save][versions][checkpoint]") {
  TempDir dir;
  const fs::path slot_file = dir.path() / "CHECKPNT.plsave";

  // The menus' stand-in state, before any game: nothing to keep.
  GameState empty;
  GameContext context;
  CHECK(SaveGameManager::checkpoint(
            empty, context, save::CheckpointReason::BeforeNewGame, slot_file) ==
        save::CheckpointResult::NoGame);
  CHECK(names_in(dir.path()).empty());

  // The newest checkpoint already holds this game.
  LiveGame game;
  const SaveSlot slot(slot_file);
  fs::create_directories(slot.folder());
  SlotManifest manifest;
  manifest.next_sequence = 2;
  manifest.active = 1;
  manifest.versions = {
      working_version(1, "x", hash_of(game.state, game.context))};
  REQUIRE(slot.write_manifest(manifest));
  const std::string manifest_text = read_file(slot.manifest_path());
  CHECK(SaveGameManager::checkpoint(
            game.state, game.context, save::CheckpointReason::BeforeLoad,
            slot_file) == save::CheckpointResult::Unchanged);
  CHECK(read_file(slot.manifest_path()) == manifest_text);
  CHECK(names_in(slot.folder()) == std::set<std::string>{"manifest.json"});
}

// With the original game

TEST_CASE("A slot keeps three load-verified versions of real saves",
          "[save][versions][game]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path file = dir.path() / "game.plsave";
  const SaveSlot slot(file);

  std::vector<std::string> saved;
  for (int i = 0; i < 5; ++i) {
    std::string error;
    REQUIRE(
        SaveGameManager::save_to_slot(state, context, file.string(), &error));
    saved.push_back(hash_of(state, context));
    move_on(state);
  }

  const auto manifest = SaveGameManager::versions(file.string());
  REQUIRE(manifest);
  CHECK(sequences(*manifest) == std::vector<std::uint64_t>{5, 4, 3});
  CHECK(manifest->active == std::optional<std::uint64_t>{5});
  for (const SaveVersion& v : manifest->versions) {
    CHECK(v.status == VersionStatus::Working);
    CHECK(v.level == VersionLevel::LoadVerified);
    CHECK(v.reason.empty());
    CHECK(read_file(slot.version_path(v.sequence)).size() == v.size);
  }
  CHECK(manifest->find(5)->fingerprint == saved[4]);
  CHECK(names_in(slot.folder()) ==
        std::set<std::string>{"manifest.json", "v000003.plsave",
                              "v000004.plsave", "v000005.plsave"});
  // The slot's file is the newest version, for the menus and older builds.
  CHECK(read_file(file) == read_file(slot.version_path(5)));
  CHECK(load_hash(file, context) == saved[4]);
}

TEST_CASE("Every failed save of a real game leaves the last one loadable",
          "[save][versions][game][fault]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path file = dir.path() / "game.plsave";
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string good = hash_of(state, context);
  move_on(state);

  std::optional<ScopedAtomicFault> fault;
  SECTION("disk full partway") {
    fault.emplace(
        version_files(),
        AtomicFault{.kind = AtomicFault::Kind::DiskFull, .bytes = 512});
  }
  SECTION("a truncated write") {
    fault.emplace(
        version_files(),
        AtomicFault{.kind = AtomicFault::Kind::LostTail, .bytes = 512});
  }
  SECTION("a failed rename") {
    fault.emplace(version_files(),
                  AtomicFault{.kind = AtomicFault::Kind::RenameFails});
  }
  SECTION("a failed rename of the manifest") {
    fault.emplace(named("manifest.json"),
                  AtomicFault{.kind = AtomicFault::Kind::RenameFails});
  }
  std::string error;
  CHECK_FALSE(
      SaveGameManager::save_to_slot(state, context, file.string(), &error));
  CHECK_FALSE(error.empty());
  fault.reset();

  CHECK(load_hash(file, context) == good);
  const auto manifest = SaveGameManager::versions(file.string());
  REQUIRE(manifest);
  CHECK(manifest->active == std::optional<std::uint64_t>{1});

  // The next save goes through and the slot moves on.
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  CHECK(load_hash(file, context) == hash_of(state, context));
}

TEST_CASE("A corrupted newest version falls back to the one before, and "
          "says so",
          "[save][versions][game][load]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path file = dir.path() / "game.plsave";
  const SaveSlot slot(file);
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string first = hash_of(state, context);
  move_on(state);
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string second = hash_of(state, context);

  SECTION("only the slot's file: its version's own file is used") {
    std::string bytes = read_file(file);
    bytes[bytes.size() / 2] ^= 0x01;
    write_file(file, bytes);
    std::string notice;
    CHECK(load_hash(file, context, &notice) == second);
    CHECK(notice.find("verified version") != std::string::npos);
  }
  SECTION("both copies of the newest version") {
    for (const fs::path& copy : {file, slot.version_path(2)}) {
      std::string bytes = read_file(copy);
      bytes.resize(bytes.size() / 2);
      write_file(copy, bytes);
    }
    std::string notice;
    CHECK(load_hash(file, context, &notice) == first);
    CHECK(notice.find("verified version") != std::string::npos);
  }
}

TEST_CASE("A restore cut short leaves a loadable slot",
          "[save][versions][game][fault]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path file = dir.path() / "game.plsave";
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string first = hash_of(state, context);
  move_on(state);
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string second = hash_of(state, context);

  SECTION("before its manifest is in place: nothing was restored") {
    {
      ScopedAtomicFault fault(named("manifest.json"),
                              {.kind = AtomicFault::Kind::Interrupted});
      CHECK_THROWS_AS(
          SaveGameManager::restore_version(state, context, file.string(), 1),
          util::testing::AtomicFileInterrupted);
    }
    CHECK(load_hash(file, context) == second);
  }
  SECTION("after its manifest, before the slot's file: it was restored") {
    {
      ScopedAtomicFault fault(named("game.plsave"),
                              {.kind = AtomicFault::Kind::Interrupted});
      CHECK_THROWS_AS(
          SaveGameManager::restore_version(state, context, file.string(), 1),
          util::testing::AtomicFileInterrupted);
    }
    std::string notice;
    CHECK(load_hash(file, context, &notice) == first);
    CHECK(notice.empty());
    // Undo: the newer version is still there to restore.
    REQUIRE(SaveGameManager::restore_version(state, context, file.string(), 2));
    CHECK(load_hash(file, context) == second);
  }
  SECTION("a damaged version is not restored") {
    const SaveSlot slot(file);
    std::string bytes = read_file(slot.version_path(1));
    bytes[bytes.size() / 2] ^= 0x01;
    write_file(slot.version_path(1), bytes);
    std::string error;
    CHECK_FALSE(SaveGameManager::restore_version(state, context, file.string(),
                                                 1, &error));
    CHECK_FALSE(error.empty());
    const auto manifest = SaveGameManager::versions(file.string());
    REQUIRE(manifest);
    CHECK(manifest->find(1)->status == VersionStatus::Candidate);
    CHECK(manifest->active == std::optional<std::uint64_t>{2});
    CHECK(load_hash(file, context) == second);
  }
}

TEST_CASE("Checkpoints keep the game before a load replaces it",
          "[save][versions][game][checkpoint]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path checkpoints = dir.path() / "CHECKPNT.plsave";
  const fs::path other = dir.path() / "other.plsave";

  CHECK(SaveGameManager::checkpoint(
            state, context, save::CheckpointReason::BeforeNewGame,
            checkpoints) == save::CheckpointResult::Written);
  CHECK(SaveGameManager::checkpoint(
            state, context, save::CheckpointReason::BeforeNewGame,
            checkpoints) == save::CheckpointResult::Unchanged);
  const std::string kept = hash_of(state, context);

  move_on(state);
  REQUIRE(SaveGameManager::save_to_slot(state, context, other.string()));
  move_on(state);
  const std::string unsaved = hash_of(state, context);

  // Loading the checkpoint slot itself, over unsaved progress: the progress
  // becomes the newest checkpoint, and the load still gives what the slot
  // held when it was chosen.
  std::string error;
  std::string notice;
  REQUIRE(SaveGameManager::load_over_current(
      state, context, checkpoints.string(), &error, &notice, checkpoints));
  CHECK(hash_of(state, context) == kept);
  CHECK(notice.empty());
  const auto manifest = SaveGameManager::versions(checkpoints.string());
  REQUIRE(manifest);
  REQUIRE(manifest->newest_working() != nullptr);
  CHECK(manifest->newest_working()->fingerprint == unsaved);
  CHECK(manifest->newest_working()->made_for ==
        "checkpoint before loading a save");
}

// Timed checkpoints and the Recovery Center

namespace {

GameTime at_day(int day) {
  GameTime time;
  time.time_blocks(day * GameTime::BLOCKS_PER_DAY + 9 * 6);
  return time;
}

std::unique_ptr<save::CheckpointJob>
start_timed(LiveGame& game, const fs::path& slot,
            save::CheckpointResult* result = nullptr) {
  return SaveGameManager::start_timed_checkpoint(
      game.state, game.context, save::TimedCheckpoint::Interval, result, slot);
}

} // namespace

TEST_CASE("Timed checkpoints come every five minutes of play and each new "
          "day and week",
          "[save][versions][checkpoint]") {
  save::CheckpointTimer timer;
  timer.observe(at_day(0));
  CHECK_FALSE(timer.due());

  SECTION("five minutes of play") {
    timer.played(std::chrono::minutes(4) + std::chrono::seconds(59));
    CHECK_FALSE(timer.due());
    timer.played(std::chrono::milliseconds(-5000)); // never runs backwards
    timer.played(std::chrono::seconds(1));
    CHECK(timer.due() == save::TimedCheckpoint::Interval);
    timer.taken();
    CHECK_FALSE(timer.due());
  }
  SECTION("a new day") {
    timer.observe(at_day(0));
    CHECK_FALSE(timer.due());
    timer.observe(at_day(1));
    CHECK(timer.boundary_pending());
    timer.played(std::chrono::minutes(10));
    CHECK(timer.due() == save::TimedCheckpoint::NewDay);
    timer.taken();
    CHECK_FALSE(timer.due());
  }
  SECTION("a new week") {
    int end_of_week = 0;
    while (!at_day(end_of_week).current_date().is_end_of_week()) {
      ++end_of_week;
    }
    timer.observe(at_day(end_of_week));
    timer.taken();
    timer.observe(at_day(end_of_week + 1));
    CHECK(timer.due() == save::TimedCheckpoint::NewWeek);
  }
}

TEST_CASE("A timed checkpoint that fails on its worker costs nothing",
          "[save][versions][checkpoint]") {
  TempDir dir;
  LiveGame game;
  const PreparedSlot slot = prepare_slot(dir, game);
  const SaveSlot versions(slot.file);

  // The same game as the working version: the worker finds nothing to keep,
  // and stops before writing (or building) anything.
  save::CheckpointResult result = save::CheckpointResult::Failed;
  {
    auto job = start_timed(game, slot.file, &result);
    REQUIRE(job);
    CHECK(result == save::CheckpointResult::Written);
    job->wait();
    CHECK(job->unchanged());
    CHECK_FALSE(job->ready());
    CHECK(SaveGameManager::promote(*job) == save::CheckpointResult::Unchanged);
    CHECK(read_file(versions.manifest_path()) == slot.manifest_text);
  }

  move_on(game.state);

  // Every failure here stops before the build, which needs the game's files.
  SECTION("the disk fills up on the worker") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::DiskFull, .bytes = 64});
    auto job = start_timed(game, slot.file);
    REQUIRE(job);
    job->wait();
    CHECK_FALSE(job->ready());
    CHECK_FALSE(job->error().empty());
    CHECK(SaveGameManager::promote(*job) == save::CheckpointResult::Failed);
  }
  SECTION("the disk loses the end of the version on the worker") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::LostTail, .bytes = 40});
    auto job = start_timed(game, slot.file);
    REQUIRE(job);
    job->wait();
    CHECK_FALSE(job->ready());
    CHECK(job->error().find("bytes on disk") != std::string::npos);
  }
  SECTION("the game is killed while the worker writes") {
    ScopedAtomicFault fault(version_files(),
                            {.kind = AtomicFault::Kind::Interrupted});
    auto job = start_timed(game, slot.file);
    REQUIRE(job);
    job->wait();
    CHECK_FALSE(job->ready());
    for (const auto& name : names_in(versions.folder())) {
      if (util::is_temp_file_name(name)) {
        fs::remove(versions.folder() / name);
      }
    }
  }
  require_untouched(slot);

  // There is no game in the menus' stand-in state.
  GameState empty;
  CHECK_FALSE(SaveGameManager::start_timed_checkpoint(
      empty, game.context, save::TimedCheckpoint::NewDay, &result, slot.file));
  CHECK(result == save::CheckpointResult::NoGame);
}

// Settles the background autosaves until none is left, for a bounded time.
void settle_all(GameState& state) {
  for (int i = 0; i < 1000 && SaveGameManager::background_saves() > 0; ++i) {
    SaveGameManager::settle_background_saves(state);
    if (SaveGameManager::background_saves() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  REQUIRE(SaveGameManager::background_saves() == 0);
}

TEST_CASE("A background autosave that fails keeps the last one",
          "[save][versions][autosave]") {
  TempDir dir;
  LiveGame game;
  game.state.options().autosave_count = 1;
  const PreparedSlot slot = prepare_slot(dir, game, "AUTOSAV1.plsave");
  move_on(game.state);

  // It fails on the worker, before any build.
  ScopedAtomicFault fault(version_files(),
                          {.kind = AtomicFault::Kind::DiskFull, .bytes = 64});
  REQUIRE(SaveGameManager::autosave_in_background(game.state, game.context,
                                                  dir.path()));
  CHECK(SaveGameManager::background_saves() == 1);
  settle_all(game.state);
  CHECK(fault.hits() == 1);
  require_untouched(slot);
}

TEST_CASE("The Recovery Center lists every slot with its versions",
          "[save][versions][recovery]") {
  TempDir dir;
  LiveGame game;
  const PreparedSlot slot = prepare_slot(dir, game);
  REQUIRE(SaveGameManager::save(game.state, game.context,
                                (dir.path() / "plain.plsave").string()));
  const SaveSlot lost(dir.path() / "CHECKPNT.plsave");
  fs::create_directories(lost.folder());
  write_file(lost.manifest_path(), "{ damaged");

  const auto slots = SaveGameManager::recovery_slots(dir.path());
  REQUIRE(slots.size() == 3);
  CHECK(slots[0].name == "CHECKPNT.plsave");
  CHECK(slots[0].checkpoint);
  CHECK_FALSE(slots[0].has_file);
  CHECK_FALSE(slots[0].versions);
  CHECK_FALSE(slots[0].problem.empty());

  CHECK(slots[1].name == "game.plsave");
  CHECK_FALSE(slots[1].checkpoint);
  CHECK(slots[1].has_file);
  REQUIRE(slots[1].versions);
  REQUIRE(slots[1].versions->versions.size() == 1);
  CHECK(slots[1].versions->versions[0].level == VersionLevel::LoadVerified);

  CHECK(slots[2].name == "plain.plsave");
  CHECK(slots[2].has_file);
  CHECK_FALSE(slots[2].versions);
  CHECK(slots[2].problem.empty());
}

TEST_CASE("A branch or an undo that can't be made changes nothing",
          "[save][versions][recovery]") {
  TempDir dir;
  LiveGame game;
  const PreparedSlot slot = prepare_slot(dir, game);
  const fs::path branch = dir.path() / "branch.plsave";
  std::string error;

  SECTION("a name that is taken") {
    write_file(branch, "someone's save");
    CHECK_FALSE(SaveGameManager::restore_as_branch(game.state, game.context,
                                                   slot.file.string(), 1,
                                                   branch.string(), &error));
    CHECK(error == "There is already a save of that name.");
    CHECK(read_file(branch) == "someone's save");
  }
  SECTION("a damaged version") {
    const SaveSlot versions(slot.file);
    std::string bytes = slot.bytes;
    bytes[bytes.size() / 2] ^= 0x01;
    write_file(versions.version_path(1), bytes);
    CHECK_FALSE(SaveGameManager::restore_as_branch(game.state, game.context,
                                                   slot.file.string(), 1,
                                                   branch.string(), &error));
    CHECK_FALSE(fs::exists(branch));
    CHECK_FALSE(fs::exists(SaveSlot(branch).folder()));
    write_file(versions.version_path(1), slot.bytes);
  }
  SECTION("a version that is not there") {
    CHECK_FALSE(SaveGameManager::restore_as_branch(game.state, game.context,
                                                   slot.file.string(), 9,
                                                   branch.string(), &error));
    CHECK_FALSE(fs::exists(branch));
  }
  SECTION("an undo with no restore") {
    CHECK_FALSE(SaveGameManager::undo_restore(game.state, game.context,
                                              slot.file.string(), &error));
    CHECK(error == "There is no restore of this save to undo.");
  }
  require_untouched(slot);
}

TEST_CASE("A timed checkpoint is built and checked on its worker",
          "[save][versions][game][checkpoint]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path slot = dir.path() / "AUTOCHK.plsave";
  const std::string kept = hash_of(state, context);
  const std::uint32_t game_stream = engine::random::get_state();

  auto job = SaveGameManager::start_timed_checkpoint(
      state, context, save::TimedCheckpoint::NewDay, nullptr, slot);
  REQUIRE(job);
  job->wait();
  INFO(job->error());
  REQUIRE(job->ready());
  // The build drew from the worker's own random stream.
  CHECK(engine::random::get_state() == game_stream);

  // Checked in full, waiting for the game thread.
  auto manifest = SaveGameManager::versions(slot.string());
  REQUIRE(manifest);
  const SaveVersion* waiting = manifest->find(job->sequence());
  REQUIRE(waiting != nullptr);
  CHECK(waiting->status == VersionStatus::Candidate);
  CHECK(waiting->level == VersionLevel::LoadVerified);
  CHECK(waiting->reason == save::awaiting_promotion);
  CHECK(waiting->made_for == "checkpoint at a new day");
  CHECK_FALSE(manifest->active);
  // The worker already wrote the slot's file; loading still takes the
  // active version until promotion.
  CHECK(read_file(slot) ==
        read_file(SaveSlot(slot).version_path(job->sequence())));
  // Waiting, it already counts as the newest checkpoint.
  {
    auto again = SaveGameManager::start_timed_checkpoint(
        state, context, save::TimedCheckpoint::Interval, nullptr, slot);
    REQUIRE(again);
    again->wait();
    CHECK(again->unchanged());
  }

  CHECK(SaveGameManager::promote(*job) == save::CheckpointResult::Written);
  manifest = SaveGameManager::versions(slot.string());
  REQUIRE(manifest);
  CHECK(manifest->active == std::optional<std::uint64_t>{job->sequence()});
  CHECK(manifest->find(job->sequence())->status == VersionStatus::Working);
  CHECK(load_hash(slot, context) == kept);
  // Once only.
  CHECK(SaveGameManager::promote(*job) == save::CheckpointResult::Failed);
}

TEST_CASE("The autosave is written and checked on its worker",
          "[save][versions][game][autosave]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  state.options().autosave_count = 2;
  const std::string kept = hash_of(state, context);
  const std::uint32_t game_stream = engine::random::get_state();

  REQUIRE(SaveGameManager::autosave_in_background(state, context, dir.path()));
  settle_all(state);
  CHECK(engine::random::get_state() == game_stream);

  const fs::path first = dir.path() / "AUTOSAV1.plsave";
  const auto manifest = SaveGameManager::versions(first.string());
  REQUIRE(manifest);
  CHECK(manifest->active == std::optional<std::uint64_t>{1});
  CHECK(manifest->find(1)->status == VersionStatus::Working);
  CHECK(manifest->find(1)->made_for == "autosave");
  CHECK(load_hash(first, context) == kept);

  // The next one goes to the next slot, and a save by hand to the same slot
  // waits for a background one still running.
  move_on(state);
  REQUIRE(SaveGameManager::autosave_in_background(state, context, dir.path()));
  const fs::path second = dir.path() / "AUTOSAV2.plsave";
  REQUIRE(SaveGameManager::save_to_slot(state, context, second.string()));
  CHECK(SaveGameManager::background_saves() == 0);
  const auto both = SaveGameManager::versions(second.string());
  REQUIRE(both);
  CHECK(sequences(*both) == std::vector<std::uint64_t>{2, 1});
  CHECK(both->find(1)->status == VersionStatus::Working);
  CHECK(both->find(2)->status == VersionStatus::Working);
}

TEST_CASE("TimedCheckpoints keeps the game as it is played",
          "[save][versions][game][checkpoint]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path slot = dir.path() / "AUTOCHK.plsave";
  save::TimedCheckpoints checkpoints(slot);

  checkpoints.frame(state, context, std::chrono::minutes(5), true);
  checkpoints.finish();
  const std::string first = hash_of(state, context);

  // The next day starts another one.
  GameTime next = state.time();
  next.time_blocks(next.time_blocks() + GameTime::BLOCKS_PER_DAY);
  state.set_time(next);
  checkpoints.frame(state, context, std::chrono::milliseconds(16), true);
  checkpoints.finish();
  const std::string second = hash_of(state, context);

  const auto manifest = SaveGameManager::versions(slot.string());
  REQUIRE(manifest);
  REQUIRE(manifest->find(1) != nullptr);
  CHECK(manifest->find(1)->status == VersionStatus::Working);
  CHECK(manifest->find(1)->fingerprint == first);
  REQUIRE(manifest->find(2) != nullptr);
  CHECK(manifest->find(2)->status == VersionStatus::Working);
  CHECK(manifest->find(2)->made_for == "checkpoint at a new day");
  CHECK(manifest->active == std::optional<std::uint64_t>{2});
  CHECK(load_hash(slot, context) == second);
}

TEST_CASE("Restore as a branch, and undo a restore",
          "[save][versions][game][recovery]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const fs::path file = dir.path() / "game.plsave";
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string first = hash_of(state, context);
  move_on(state);
  REQUIRE(SaveGameManager::save_to_slot(state, context, file.string()));
  const std::string second = hash_of(state, context);
  const std::string manifest_before = read_file(SaveSlot(file).manifest_path());

  const fs::path branch = dir.path() / "branch.plsave";
  REQUIRE(SaveGameManager::restore_as_branch(state, context, file.string(), 1,
                                             branch.string()));
  CHECK(load_hash(branch, context) == first);
  const auto branched = SaveGameManager::versions(branch.string());
  REQUIRE(branched);
  CHECK(sequences(*branched) == std::vector<std::uint64_t>{1});
  CHECK(branched->find(1)->made_for ==
        "branch of version 1 of " + file.string());
  // The slot it came from is as it was.
  CHECK(read_file(SaveSlot(file).manifest_path()) == manifest_before);
  CHECK(load_hash(file, context) == second);

  REQUIRE(SaveGameManager::restore_version(state, context, file.string(), 1));
  CHECK(load_hash(file, context) == first);
  REQUIRE(SaveGameManager::undo_restore(state, context, file.string()));
  CHECK(load_hash(file, context) == second);
  // And the undo can be undone.
  REQUIRE(SaveGameManager::undo_restore(state, context, file.string()));
  CHECK(load_hash(file, context) == first);
}

// What a checkpoint costs on the game thread, for the frame budget. Hidden:
//   PLEGACY_TEST_GAME_PATH=... test_plegacy "[bench][checkpoint]"
TEST_CASE("Checkpoint costs", "[.][bench][checkpoint]") {
  REQUIRE_GAME_DATA();
  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  constexpr int rounds = 5;
  double capture = 0;
  double dump = 0;
  double fingerprint = 0;
  std::size_t json_bytes = 0;
  double plain = 0;
  double slot = 0;
  double load = 0;
  double start_job = 0;
  double worker = 0;
  double promote = 0;
  for (int i = 0; i < rounds; ++i) {
    move_on(state);
    // The game thread's share, in parts: building the JSON, then what a
    // fingerprint adds (the dump and the hash), which now runs on the worker.
    auto t = clock::now();
    const json captured = SaveGameManager::capture(state, context);
    capture += ms(clock::now() - t);
    t = clock::now();
    const std::string dumped = captured.dump();
    dump += ms(clock::now() - t);
    t = clock::now();
    static_cast<void>(SaveGameManager::fingerprint(state, context));
    fingerprint += ms(clock::now() - t);
    json_bytes = dumped.size();
    t = clock::now();
    REQUIRE(SaveGameManager::save(state, context,
                                  (dir.path() / "plain.plsave").string()));
    plain += ms(clock::now() - t);
    t = clock::now();
    REQUIRE(SaveGameManager::save_to_slot(
        state, context, (dir.path() / "slot.plsave").string()));
    slot += ms(clock::now() - t);
    GameState loaded;
    t = clock::now();
    REQUIRE(SaveGameManager::load(loaded, context,
                                  (dir.path() / "plain.plsave").string()));
    load += ms(clock::now() - t);

    // A timed checkpoint: what the game thread pays, and the worker.
    const fs::path timed = dir.path() / "AUTOCHK.plsave";
    t = clock::now();
    auto job = SaveGameManager::start_timed_checkpoint(
        state, context, save::TimedCheckpoint::Interval, nullptr, timed);
    start_job += ms(clock::now() - t);
    REQUIRE(job);
    job->wait();
    worker += ms(clock::now() - t);
    REQUIRE(job->ready());
    t = clock::now();
    REQUIRE(SaveGameManager::promote(*job) == save::CheckpointResult::Written);
    promote += ms(clock::now() - t);
  }
  WARN("per checkpoint, ms: capture (state to JSON, the game thread's only "
       << "part now) " << capture / rounds << "; its dump " << dump / rounds
       << " (" << json_bytes << " bytes); a whole fingerprint "
       << fingerprint / rounds << "; plain save " << plain / rounds
       << "; verified slot save " << slot / rounds
       << "; so the load check about " << (slot - plain) / rounds
       << "; a load (two builds) " << load / rounds
       << "; timed checkpoint on the game thread: start " << start_job / rounds
       << " + promote " << promote / rounds << ", on its worker "
       << worker / rounds << "; frame budget 16");
}
