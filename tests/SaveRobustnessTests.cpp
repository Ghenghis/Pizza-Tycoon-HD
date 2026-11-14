/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Saving and loading can't corrupt anything. AtomicFile replaces a file in
// one step or not at all, and a save that fails any check is turned away
// before the running game is touched.
//
// Most of these run without the original game: the game they save is built
// from a hand-made character, and every damaged save is turned away before
// loading would need the game's files. The ones that load a save for real
// SKIP without PLEGACY_TEST_GAME_PATH.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "TestSupport.h"
#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/save/SaveGameManager.h"
#include "core/state/GameState.h"
#include "core/time/GameTime.h"
#include "core/util/AtomicFile.h"
#include "core/util/ConfigManager.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;
using save::SaveGameManager;
using test_support::TempDir;

namespace {

// What the player is told.
constexpr std::string_view damaged =
    "This save file is damaged and cannot be loaded.";
constexpr std::string_view newer =
    "This save was made by a newer version of the game and cannot be loaded.";
constexpr std::string_view unreadable = "The save file could not be read.";

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

// What a folder holds, to show nothing was left behind.
std::set<std::string> names_in(const fs::path& dir) {
  std::set<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    names.insert(entry.path().filename().string());
  }
  return names;
}

void put_le(std::string& out, std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

// A zlib stream holding the data in stored, uncompressed blocks. zlib's
// uncompress() reads it like any other, so the tests write save files of
// their own without needing zlib.
std::string zlib_stored(std::string_view data) {
  std::string out("\x78\x01", 2);
  std::size_t pos = 0;
  do {
    const std::size_t length = std::min<std::size_t>(data.size() - pos, 0xFFFF);
    const bool last = pos + length == data.size();
    out.push_back(last ? '\x01' : '\x00');
    put_le(out, length, 2);
    put_le(out, ~length & 0xFFFF, 2);
    out.append(data.substr(pos, length));
    pos += length;
  } while (pos < data.size());

  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const char c : data) {
    a = (a + static_cast<unsigned char>(c)) % 65521;
    b = (b + a) % 65521;
  }
  const std::uint32_t adler = (b << 16) | a;
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((adler >> shift) & 0xFF));
  }
  return out;
}

// The parts of a save file, for writing damaged ones on purpose. The layout
// is the one SaveGameManager reads: "PLSAVE", the header version and the
// inflated size, then (header version 2) a metadata block, then the zlib
// stream.
struct RawSave {
  std::string game_data;
  std::uint16_t header_version = 2;
  std::string metadata = R"({"saved_at":"test"})";
  // What the header says the game data inflates to, if not its real size.
  std::optional<std::uint64_t> claimed_size = std::nullopt;
};

std::string encode(const RawSave& save) {
  std::string out = "PLSAVE";
  put_le(out, save.header_version, 2);
  put_le(out, save.claimed_size.value_or(save.game_data.size()), 8);
  if (save.header_version >= 2) {
    put_le(out, save.metadata.size(), 4);
    out += save.metadata;
  }
  out += zlib_stored(save.game_data);
  return out;
}

// A game in progress built without the original game's files: two players
// sharing a hand-made character, and no city yet.
struct LiveGame {
  core::character::CharacterData character{}; // outlives the players
  GameState state;
  GameContext context;

  LiveGame() {
    std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
    for (int i = 0; i < 2; ++i) {
      players[i] = std::make_unique<Player>(
          core::PlayerId{i}, core::character::Character(&character), i == 0);
      players[i]->name("Player " + std::to_string(i + 1));
    }
    state.players(std::move(players));
    state.active_player(core::PlayerId{0});
    GameTime time;
    time.time_blocks(9 * 6 + 40);
    state.set_time(time);
    state.difficulty_level(4);
    state.price_update_category(2);
    engine::random::set_state(0x5EED1234);
  }

  LiveGame(const LiveGame&) = delete;
  LiveGame& operator=(const LiveGame&) = delete;

  // The document save() writes for this game.
  [[nodiscard]] json document() const {
    json j = SaveGameManager::fingerprint(state, context).document;
    j["saved_at"] = "test";
    return j;
  }
};

// What a turned-away load must leave as it was.
struct Snapshot {
  std::string fingerprint; // the whole saved state, random state included
  std::uint32_t random_state;
  std::size_t ui_depth;
};

Snapshot snapshot(const LiveGame& game) {
  return {SaveGameManager::fingerprint(game.state, game.context).hash,
          engine::random::get_state(), game.state.ui().size()};
}

void require_rejected(LiveGame& game, const fs::path& file,
                      std::string_view message) {
  const Snapshot before = snapshot(game);
  std::string error;
  CHECK_FALSE(
      SaveGameManager::load(game.state, game.context, file.string(), &error));
  CHECK(error == message);
  const Snapshot after = snapshot(game);
  CHECK(after.fingerprint == before.fingerprint);
  CHECK(after.random_state == before.random_state);
  CHECK(after.ui_depth == before.ui_depth);
}

// Writes the document as a save and checks loading it is turned away. Every
// document given here fails a check before loading would build a player,
// which needs the original game's files.
void require_document_rejected(LiveGame& game, const TempDir& dir,
                               const json& document,
                               std::string_view message = damaged) {
  const fs::path file = dir.path() / "damaged.plsave";
  write_file(file, encode(RawSave{.game_data = document.dump()}));
  CHECK_FALSE(SaveGameManager::is_valid_save(file.string()));
  require_rejected(game, file, message);
}

// A file under tests/fixtures/saves, found from the source tree or the build.
fs::path fixture(std::string_view name) {
  const fs::path beside_source =
      fs::path(__FILE__).parent_path() / "fixtures" / "saves" / name;
  if (fs::exists(beside_source)) {
    return beside_source;
  }
  for (const char* base : {"tests/fixtures/saves", "../tests/fixtures/saves",
                           "../../tests/fixtures/saves"}) {
    if (fs::exists(fs::path(base) / name)) {
      return fs::path(base) / name;
    }
  }
  return beside_source;
}

// Version 1 numbered a premise for rent before one for sale.
void swap_rent_and_sale(json& document) {
  for (json& city : document["cities"]) {
    for (json& property : city["properties"]) {
      if (!property.contains("status")) {
        continue;
      }
      const int status = property["status"].get<int>();
      if (status == 1 || status == 2) {
        property["status"] = 3 - status;
      }
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

// Checks two fingerprints match, listing the first fields that moved if not.
void require_same_game(const save::StateFingerprint& expected,
                       const save::StateFingerprint& actual) {
  if (expected.hash != actual.hash) {
    const std::string moved =
        json::diff(expected.document, actual.document).dump();
    INFO("fields that moved: " << moved.substr(0, 4000));
    CHECK(actual.hash == expected.hash);
  }
}

#ifndef _WIN32
// Puts a folder's permissions back however the test ends, so TempDir can
// remove it.
class ScopedPermissions {
public:
  ScopedPermissions(fs::path path, fs::perms perms)
      : path_(std::move(path)), saved_(fs::status(path_).permissions()) {
    fs::permissions(path_, perms);
  }
  ~ScopedPermissions() {
    std::error_code ec;
    fs::permissions(path_, saved_, ec);
  }
  ScopedPermissions(const ScopedPermissions&) = delete;
  ScopedPermissions& operator=(const ScopedPermissions&) = delete;

private:
  fs::path path_;
  fs::perms saved_;
};
#endif

#if defined(__linux__)
// While alive, no file can grow past `bytes`: a write beyond it fails with
// EFBIG, the way a full disk fails with ENOSPC. The SIGXFSZ the kernel also
// sends would end the process, so it is ignored meanwhile. Keep the scope to
// the write under test; nothing else should write files while it lasts.
class FileSizeLimit {
public:
  explicit FileSizeLimit(rlim_t bytes) {
    REQUIRE(getrlimit(RLIMIT_FSIZE, &saved_) == 0);
    saved_handler_ = std::signal(SIGXFSZ, SIG_IGN);
    rlimit limited = saved_;
    limited.rlim_cur = bytes;
    active_ = setrlimit(RLIMIT_FSIZE, &limited) == 0;
  }
  ~FileSizeLimit() {
    setrlimit(RLIMIT_FSIZE, &saved_);
    std::signal(SIGXFSZ, saved_handler_);
  }
  FileSizeLimit(const FileSizeLimit&) = delete;
  FileSizeLimit& operator=(const FileSizeLimit&) = delete;

  [[nodiscard]] bool active() const { return active_; }

private:
  rlimit saved_{};
  void (*saved_handler_)(int) = SIG_DFL;
  bool active_ = false;
};
#endif

} // namespace

// AtomicFile

TEST_CASE("AtomicFile writes a new file and leaves nothing else",
          "[save][atomic]") {
  TempDir dir;
  const fs::path file = dir.path() / "settings.conf";

  const util::AtomicWriteResult result =
      util::write_file_atomically(file, "fresh\n");
  REQUIRE(result);
  CHECK(result.error.empty());
  CHECK(read_file(file) == "fresh\n");
  CHECK(names_in(dir.path()) == std::set<std::string>{"settings.conf"});
}

TEST_CASE("AtomicFile replaces the old contents whole", "[save][atomic]") {
  TempDir dir;
  const fs::path file = dir.path() / "save.plsave";
  write_file(file, "the old contents, longer than the new ones\n");

  REQUIRE(util::write_file_atomically(file, "new\n"));
  CHECK(read_file(file) == "new\n");
  CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
}

TEST_CASE("AtomicFile changes nothing until it commits", "[save][atomic]") {
  TempDir dir;
  const fs::path file = dir.path() / "save.plsave";
  write_file(file, "old\n");

  SECTION("dropped without a commit") {
    {
      util::AtomicFile out(file);
      REQUIRE(out.open());
      // The new bytes go beside the destination, under a name of their own.
      CHECK(out.temp_path().parent_path() == file.parent_path());
      CHECK(
          out.temp_path().filename().string().starts_with("save.plsave.tmp-"));
      CHECK(fs::exists(out.temp_path()));
      REQUIRE(out.write("half a save"));
      CHECK(read_file(file) == "old\n");
    }
    CHECK(read_file(file) == "old\n");
    CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
  }

  SECTION("discarded") {
    util::AtomicFile out(file);
    REQUIRE(out.open());
    REQUIRE(out.write("thrown away"));
    out.discard();
    CHECK(out.temp_path().empty());
    CHECK(read_file(file) == "old\n");
    CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
  }
}

TEST_CASE("AtomicFile can't replace a folder, and says so", "[save][atomic]") {
  TempDir dir;
  const fs::path target = dir.path() / "slot.plsave";
  fs::create_directory(target);
  write_file(target / "inside", "x");

  const util::AtomicWriteResult result =
      util::write_file_atomically(target, "data");
  CHECK_FALSE(result);
  CHECK(result.error.find("slot.plsave") != std::string::npos);
  CHECK(fs::is_directory(target));
  CHECK(read_file(target / "inside") == "x");
  CHECK(names_in(dir.path()) == std::set<std::string>{"slot.plsave"});
}

TEST_CASE("AtomicFile reports a folder that is missing or not a folder",
          "[save][atomic]") {
  TempDir dir;

  const util::AtomicWriteResult missing =
      util::write_file_atomically(dir.path() / "no-such-folder" / "f", "data");
  CHECK_FALSE(missing);
  CHECK(missing.code == std::errc::no_such_file_or_directory);
  CHECK(missing.error.find("no-such-folder") != std::string::npos);

  write_file(dir.path() / "plain-file", "x");
  const util::AtomicWriteResult blocked =
      util::write_file_atomically(dir.path() / "plain-file" / "f", "data");
  CHECK_FALSE(blocked);
  CHECK_FALSE(blocked.error.empty());
  CHECK(read_file(dir.path() / "plain-file") == "x");
  CHECK(names_in(dir.path()) == std::set<std::string>{"plain-file"});
}

TEST_CASE("AtomicFile keeps the first failure and refuses to go on",
          "[save][atomic]") {
  TempDir dir;
  util::AtomicFile out(dir.path() / "missing" / "file");
  CHECK_FALSE(out.open());
  CHECK(out.failed());
  const std::string first = out.error();
  CHECK_FALSE(first.empty());
  CHECK_FALSE(out.write("more"));
  CHECK_FALSE(out.commit());
  CHECK(out.error() == first);
}

TEST_CASE("AtomicFile keeps the old file when a write fails partway",
          "[save][atomic]") {
#if defined(__linux__)
  TempDir dir;
  const fs::path file = dir.path() / "save.plsave";
  write_file(file, "the good save\n");

  util::AtomicWriteResult result;
  {
    FileSizeLimit limit(1024);
    REQUIRE(limit.active());
    result = util::write_file_atomically(file, std::string(64 * 1024, 'x'));
  }
  CHECK_FALSE(result);
  CHECK(result.code == std::errc::file_too_large);
  CHECK_FALSE(result.error.empty());
  CHECK(read_file(file) == "the good save\n");
  CHECK(names_in(dir.path()) == std::set<std::string>{"save.plsave"});
#else
  SKIP("Fails the write with RLIMIT_FSIZE, which is Linux only here");
#endif
}

TEST_CASE("AtomicFile keeps the old file in a folder it can't write",
          "[save][atomic]") {
#ifndef _WIN32
  if (geteuid() == 0) {
    SKIP("Root writes to read-only folders anyway");
  }
  TempDir dir;
  const fs::path folder = dir.path() / "locked";
  fs::create_directory(folder);
  write_file(folder / "save.plsave", "old\n");

  util::AtomicWriteResult result;
  {
    ScopedPermissions locked(folder,
                             fs::perms::owner_read | fs::perms::owner_exec);
    result = util::write_file_atomically(folder / "save.plsave", "new\n");
  }
  CHECK_FALSE(result);
  CHECK(result.code == std::errc::permission_denied);
  CHECK(read_file(folder / "save.plsave") == "old\n");
  CHECK(names_in(folder) == std::set<std::string>{"save.plsave"});
#else
  SKIP("Windows ignores the read-only flag on folders");
#endif
}

TEST_CASE("AtomicFile writes through a link and keeps the link",
          "[save][atomic]") {
#ifndef _WIN32
  TempDir dir;
  const fs::path real = dir.path() / "real.conf";
  const fs::path link = dir.path() / "link.conf";
  write_file(real, "old\n");
  fs::create_symlink(real, link);

  REQUIRE(util::write_file_atomically(link, "new\n"));
  CHECK(fs::is_symlink(link));
  CHECK(read_file(real) == "new\n");
  CHECK(names_in(dir.path()) ==
        std::set<std::string>{"link.conf", "real.conf"});
#else
  SKIP("Making links on Windows needs extra rights");
#endif
}

TEST_CASE("Settings are written whole or not at all", "[save][atomic]") {
  TempDir dir;
  const fs::path file = dir.path() / "settings.conf";

  ConfigManager config(file.string());
  config.active_game_path("C:/Games/PIZZA");
  REQUIRE(config.save_config());

  ConfigManager reread(file.string());
  REQUIRE(reread.load_config());
  CHECK(reread.active_game_path() == "C:/Games/PIZZA");
  CHECK(names_in(dir.path()) == std::set<std::string>{"settings.conf"});

  // A folder in the way: the write fails and says so, instead of ending the
  // game as it used to.
  const fs::path blocked = dir.path() / "blocked.conf";
  fs::create_directory(blocked);
  ConfigManager unwritable(blocked.string());
  CHECK_FALSE(unwritable.save_config());
  CHECK(fs::is_directory(blocked));
}

// Saving

TEST_CASE("A save the game writes passes every check the loader makes",
          "[save]") {
  TempDir dir;
  LiveGame game;
  const fs::path file = dir.path() / "game.plsave";

  std::string error = "untouched";
  REQUIRE(
      SaveGameManager::save(game.state, game.context, file.string(), &error));
  CHECK(error == "untouched");
  CHECK(SaveGameManager::is_valid_save(file.string()));
  CHECK(SaveGameManager::get_save_version(file.string()) ==
        save::SAVE_FORMAT_VERSION);

  const auto meta = SaveGameManager::read_save_metadata(file.string());
  REQUIRE(meta);
  CHECK(meta->player_names[0] == "Player 1");
  CHECK(meta->player_names[1] == "Player 2");
  CHECK(meta->active_player == 0);

  // Saving over it replaces it and leaves nothing else.
  REQUIRE(SaveGameManager::save(game.state, game.context, file.string()));
  CHECK(SaveGameManager::is_valid_save(file.string()));
  CHECK(names_in(dir.path()) == std::set<std::string>{"game.plsave"});

  // A mission in progress passes too.
  json with_mission = game.document();
  with_mission["mission_target"] = {
      {"source_name", "TEST.MIS"},
      {"milestones", {{{"id", 5}, {"y", 1995}, {"m", 2}, {"d", 28}}}}};
  const fs::path mission_file = dir.path() / "mission.plsave";
  write_file(mission_file, encode(RawSave{.game_data = with_mission.dump()}));
  CHECK(SaveGameManager::is_valid_save(mission_file.string()));
}

TEST_CASE("A save that can't be written changes no file and says why",
          "[save]") {
  TempDir dir;
  LiveGame game;

  SECTION("a folder where the save goes") {
    const fs::path slot = dir.path() / "slot.plsave";
    fs::create_directory(slot);
    std::string error;
    CHECK_FALSE(
        SaveGameManager::save(game.state, game.context, slot.string(), &error));
    CHECK(error.starts_with("The game could not be saved"));
    CHECK(error.ends_with("No save file was changed."));
    CHECK(fs::is_directory(slot));
    CHECK(names_in(dir.path()) == std::set<std::string>{"slot.plsave"});
  }

  SECTION("a file where the save folder goes") {
    write_file(dir.path() / "not-a-folder", "x");
    std::string error;
    CHECK_FALSE(SaveGameManager::save(
        game.state, game.context,
        (dir.path() / "not-a-folder" / "game.plsave").string(), &error));
    CHECK(error.starts_with("The game could not be saved"));
    CHECK(read_file(dir.path() / "not-a-folder") == "x");
  }

#if defined(__linux__)
  SECTION("the disk fills up partway, over a good save") {
    const fs::path file = dir.path() / "game.plsave";
    REQUIRE(SaveGameManager::save(game.state, game.context, file.string()));
    const std::string good = read_file(file);
    REQUIRE(good.size() > 256);

    std::string error;
    bool saved = true;
    {
      FileSizeLimit limit(64);
      REQUIRE(limit.active());
      saved = SaveGameManager::save(game.state, game.context, file.string(),
                                    &error);
    }
    CHECK_FALSE(saved);
    CHECK(error.starts_with("The game could not be saved"));
    CHECK(read_file(file) == good);
    CHECK(names_in(dir.path()) == std::set<std::string>{"game.plsave"});
  }
#endif
}

// Loading damaged saves

TEST_CASE("A truncated save is turned away and the game carries on",
          "[save][load]") {
  TempDir dir;
  LiveGame game;
  const fs::path good = dir.path() / "good.plsave";
  REQUIRE(SaveGameManager::save(game.state, game.context, good.string()));
  const std::string bytes = read_file(good);
  REQUIRE(bytes.size() > 64);

  // Empty, inside the header, inside the metadata length, partway through,
  // and one byte short of the zlib checksum.
  for (const std::size_t length :
       {std::size_t{0}, std::size_t{10}, std::size_t{17}, bytes.size() / 2,
        bytes.size() - 1}) {
    INFO("cut to " << length << " of " << bytes.size() << " bytes");
    const fs::path cut = dir.path() / "cut.plsave";
    write_file(cut, std::string_view(bytes).substr(0, length));
    CHECK_FALSE(SaveGameManager::is_valid_save(cut.string()));
    require_rejected(game, cut, damaged);
  }
}

TEST_CASE("An oversized save is turned away before it is read",
          "[save][load]") {
  TempDir dir;
  LiveGame game;
  const std::string game_data = game.document().dump();

  SECTION("the file is larger than any save") {
    const fs::path big = dir.path() / "big.plsave";
    write_file(big, encode(RawSave{.game_data = game_data}));
    // Sparse where the file system allows it.
    fs::resize_file(big, save::MAX_SAVE_FILE_SIZE + 1);
    require_rejected(game, big, damaged);
  }

  SECTION("the header claims more game data than a save holds") {
    const fs::path file = dir.path() / "claims.plsave";
    write_file(file,
               encode(RawSave{.game_data = game_data,
                              .claimed_size = save::MAX_SAVE_JSON_SIZE + 1}));
    require_rejected(game, file, damaged);
  }

  SECTION("the header claims a different size than the data has") {
    const fs::path file = dir.path() / "short.plsave";
    write_file(file, encode(RawSave{.game_data = game_data,
                                    .claimed_size = game_data.size() + 1}));
    require_rejected(game, file, damaged);
  }

  SECTION("the metadata block is longer than any") {
    std::string bytes = encode(RawSave{.game_data = game_data});
    // The block's length sits right after the 16-byte header.
    std::string length;
    put_le(length, 1024 * 1024 + 1, 4);
    bytes.replace(16, 4, length);
    const fs::path file = dir.path() / "meta.plsave";
    write_file(file, bytes);
    require_rejected(game, file, damaged);
  }

  SECTION("the game data nests deeper than any save") {
    const int depth = save::MAX_SAVE_JSON_DEPTH + 8;
    const std::string nested =
        std::string(static_cast<std::size_t>(depth), '[') +
        std::string(static_cast<std::size_t>(depth), ']');
    const fs::path file = dir.path() / "deep.plsave";
    write_file(file, encode(RawSave{.game_data = nested}));
    require_rejected(game, file, damaged);
  }
}

TEST_CASE("Garbage is turned away and the game carries on", "[save][load]") {
  TempDir dir;
  LiveGame game;
  std::mt19937 random(1994);
  std::uniform_int_distribution<int> byte(0, 255);
  std::string noise(4096, '\0');
  for (char& c : noise) {
    c = static_cast<char>(byte(random));
  }

  SECTION("no save header at all") {
    const fs::path file = dir.path() / "noise.plsave";
    write_file(file, noise);
    require_rejected(game, file, damaged);
  }

  SECTION("a save header, then noise where the game data should be") {
    std::string bytes = "PLSAVE";
    put_le(bytes, 2, 2);
    put_le(bytes, 100, 8);
    put_le(bytes, 0, 4);
    bytes += noise;
    const fs::path file = dir.path() / "noisy.plsave";
    write_file(file, bytes);
    require_rejected(game, file, damaged);
  }

  SECTION("a missing file") {
    require_rejected(game, dir.path() / "never-saved.plsave", unreadable);
  }
}

TEST_CASE("A valid header with bad JSON is turned away", "[save][load]") {
  TempDir dir;
  LiveGame game;
  for (const char* text :
       {R"({"version": 3, "time": )", "not json at all", "[1, 2, 3]", "null",
        R"({"version": "3"})", R"({"version": 3.5})"}) {
    INFO("game data: " << text);
    const fs::path file = dir.path() / "bad-json.plsave";
    write_file(file, encode(RawSave{.game_data = text}));
    CHECK_FALSE(SaveGameManager::is_valid_save(file.string()));
    require_rejected(game, file, damaged);
  }
}

TEST_CASE("A save missing what the loader needs is turned away",
          "[save][load]") {
  TempDir dir;
  LiveGame game;
  const json good = game.document();

  for (const char* key : {"time", "players", "active_player"}) {
    INFO("without " << key);
    json document = good;
    document.erase(key);
    require_document_rejected(game, dir, document);
  }
  for (const char* key : {"id", "name", "human_controlled", "character"}) {
    INFO("without players[0]." << key);
    json document = good;
    document["players"][0].erase(key);
    require_document_rejected(game, dir, document);
  }
  {
    INFO("without the version");
    json document = good;
    document.erase("version");
    require_document_rejected(game, dir, document);
  }
}

TEST_CASE("A mission with no milestones, or bad ones, is turned away",
          "[save][load]") {
  TempDir dir;
  LiveGame game;
  json document = game.document();
  document["game_mode"] = static_cast<int>(GameMode::MISSION_MODE);

  SECTION("no milestones") {
    document["mission_target"] = {{"source_name", "EMPTY.MIS"},
                                  {"milestones", json::array()}};
    require_document_rejected(game, dir, document);
  }
  SECTION("milestones that are not a list") {
    document["mission_target"] = {{"source_name", "BAD.MIS"},
                                  {"milestones", 7}};
    require_document_rejected(game, dir, document);
  }
  SECTION("no milestones key") {
    document["mission_target"] = {{"source_name", "BAD.MIS"}};
    require_document_rejected(game, dir, document);
  }
  SECTION("a month that does not exist") {
    document["mission_target"] = {
        {"milestones", {{{"id", 5}, {"y", 1995}, {"m", 13}, {"d", 1}}}}};
    require_document_rejected(game, dir, document);
  }
  SECTION("a day past the end of its month") {
    document["mission_target"] = {
        {"milestones", {{{"id", 5}, {"y", 1995}, {"m", 2}, {"d", 30}}}}};
    require_document_rejected(game, dir, document);
  }
  SECTION("a milestone that does not exist") {
    document["mission_target"] = {
        {"milestones", {{{"id", 0}, {"y", 1995}, {"m", 1}, {"d", 1}}}}};
    require_document_rejected(game, dir, document);
  }
}

TEST_CASE("IDs out of range and broken references are turned away",
          "[save][load]") {
  TempDir dir;
  LiveGame game;
  const json good = game.document();

  const auto with = [&good](const auto& change) {
    json document = good;
    change(document);
    return document;
  };

  SECTION("the active player is not there") {
    require_document_rejected(game, dir,
                              with([](json& d) { d["active_player"] = 2; }));
    require_document_rejected(game, dir,
                              with([](json& d) { d["active_player"] = 8; }));
    require_document_rejected(game, dir,
                              with([](json& d) { d["active_player"] = -1; }));
  }
  SECTION("more players than the game has") {
    require_document_rejected(
        game, dir, with([](json& d) { d["players"].push_back(nullptr); }));
  }
  SECTION("a player out of place") {
    require_document_rejected(game, dir,
                              with([](json& d) { d["players"][1]["id"] = 0; }));
  }
  SECTION("a city that does not exist, or twice") {
    require_document_rejected(game, dir, with([](json& d) {
                                d["cities"] = {{{"id", NUMBER_OF_CITIES},
                                                {"properties", json::array()}}};
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                const json city = {
                                    {"id", 3}, {"properties", json::array()}};
                                d["cities"] = {city, city};
                              }));
  }
  SECTION("a premise owned by a player that does not exist") {
    require_document_rejected(
        game, dir, with([](json& d) {
          d["cities"] = {
              {{"id", 0},
               {"properties", {{{"index", 1}, {"owner_or_tenant", 12}}}}}};
        }));
  }
  SECTION("restaurants in no city, or with one id") {
    const json restaurant = {
        {"id", 4}, {"property_city_id", 0}, {"property_index", 1}};
    require_document_rejected(game, dir, with([&restaurant](json& d) {
                                d["restaurants"] = {restaurant, restaurant};
                              }));
    require_document_rejected(game, dir, with([&restaurant](json& d) {
                                json elsewhere = restaurant;
                                elsewhere["property_city_id"] =
                                    NUMBER_OF_CITIES;
                                d["restaurants"] = {elsewhere};
                              }));
  }
  SECTION("queued events for players and kinds that do not exist") {
    require_document_rejected(game, dir, with([](json& d) {
                                d["deferred_calls"] = {
                                    {{"kind", 2}, {"player_id", 8}}};
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                d["deferred_calls"] = {{{"kind", 9}}};
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                d["deferred_staff_events"] = {
                                    {{"restaurant_id", 0},
                                     {"type", 99},
                                     {"job_role", 0},
                                     {"slot_index", 0}}};
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                d["deferred_racket_offers"] = {
                                    {{"player_id", -3},
                                     {"restaurant_id", 0},
                                     {"first_contact", true}}};
                              }));
  }
  SECTION("a CV for a player that does not exist") {
    require_document_rejected(game, dir, with([](json& d) {
                                d["shown_messages"] = {{"9", json::array()}};
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                d["shown_messages"] = {{"one", json::array()}};
                              }));
  }
  SECTION("flags and counters of the wrong size or range") {
    require_document_rejected(game, dir, with([](json& d) {
                                d["city_initialised"] = json::array({true});
                              }));
    require_document_rejected(game, dir, with([](json& d) {
                                d["player_cities"] = {NUMBER_OF_CITIES};
                              }));
    require_document_rejected(game, dir,
                              with([](json& d) { d["difficulty_level"] = 0; }));
    require_document_rejected(game, dir,
                              with([](json& d) { d["game_mode"] = 7; }));
    require_document_rejected(
        game, dir, with([](json& d) { d["rng_state"] = 1ULL << 32; }));
    require_document_rejected(
        game, dir, with([](json& d) { d["pit_tick_in_second"] = 18; }));
  }
}

TEST_CASE("A save from a newer build is turned away as such", "[save][load]") {
  TempDir dir;
  LiveGame game;

  json document = game.document();
  document["version"] = save::SAVE_FORMAT_VERSION + 1;
  require_document_rejected(game, dir, document, newer);

  const fs::path file = dir.path() / "new-header.plsave";
  write_file(file, encode(RawSave{.game_data = game.document().dump(),
                                  .header_version = 3}));
  require_rejected(game, file, newer);
}

TEST_CASE("Damage found only while building leaves the game as it was",
          "[save][load]") {
  // The checks look at structure; a mistyped rule passes them and only fails
  // once the loader reads it. The loader reads rules before players, so this
  // also runs without the original game.
  TempDir dir;
  LiveGame game;
  json document = game.document();
  document["rules"]["police"] = "yes";
  const fs::path file = dir.path() / "bad-rule.plsave";
  write_file(file, encode(RawSave{.game_data = document.dump()}));

  CHECK(SaveGameManager::is_valid_save(file.string()));
  engine::random::set_state(0xC0FFEE);
  require_rejected(game, file, damaged);
  CHECK(engine::random::get_state() == 0xC0FFEE);
}

// Older saves

TEST_CASE("A legacy save still reads", "[save][legacy]") {
  const fs::path legacy = fixture("legacy_v1.plsave");
  REQUIRE(fs::exists(legacy));

  // legacy_v1.plsave is header version 1 (no metadata block) holding save
  // format 1: one player, "Legacy Pizza", and city 0 with premise 0 at the
  // version 1 status 1, for rent.
  CHECK(SaveGameManager::is_valid_save(legacy.string()));
  CHECK(SaveGameManager::get_save_version(legacy.string()) == 1);
  const auto meta = SaveGameManager::read_save_metadata(legacy.string());
  REQUIRE(meta);
  CHECK(meta->saved_at == "Sat Mar  4 12:00:00 1995");
  CHECK(meta->game_date == "1-Jan-1995 9:00");
  CHECK(meta->player_names[0] == "Legacy Pizza");
  CHECK(meta->active_player == 0);
  CHECK(meta->active_city == 0);

  // The same layouts, made from this build's game.
  TempDir dir;
  LiveGame game;
  for (const int version : {1, 2}) {
    INFO("save format " << version);
    json document = game.document();
    document["version"] = version;
    const json metadata = {{"saved_at", "test"},
                           {"player_names", {"Player 1", "Player 2"}}};
    const fs::path file = dir.path() / "old.plsave";
    write_file(file, encode(RawSave{.game_data = document.dump(),
                                    .header_version =
                                        static_cast<std::uint16_t>(version),
                                    .metadata = metadata.dump()}));
    CHECK(SaveGameManager::is_valid_save(file.string()));
    CHECK(SaveGameManager::get_save_version(file.string()) == version);
    const auto old_meta = SaveGameManager::read_save_metadata(file.string());
    REQUIRE(old_meta);
    CHECK(old_meta->player_names[0] == "Player 1");
  }
}

// With the original game

TEST_CASE("A save loads back to the same game", "[save][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  TempDir dir;
  GameContext context;
  GameState original;
  start_free_game(original, context);
  const save::StateFingerprint before =
      SaveGameManager::fingerprint(original, context);

  const fs::path file = dir.path() / "round-trip.plsave";
  REQUIRE(SaveGameManager::save(original, context, file.string()));

  GameState loaded;
  std::string error;
  REQUIRE(SaveGameManager::load(loaded, context, file.string(), &error));
  CHECK(error.empty());
  require_same_game(before, SaveGameManager::fingerprint(loaded, context));

  // The same game written in the version 1 layout loads to the same state:
  // the premise statuses are numbered back.
  json legacy = before.document;
  legacy["version"] = 1;
  legacy["saved_at"] = "test";
  swap_rent_and_sale(legacy);
  const fs::path legacy_file = dir.path() / "legacy.plsave";
  write_file(legacy_file,
             encode(RawSave{.game_data = legacy.dump(), .header_version = 1}));
  GameState from_legacy;
  REQUIRE(SaveGameManager::load(from_legacy, context, legacy_file.string()));
  require_same_game(before, SaveGameManager::fingerprint(from_legacy, context));
}

TEST_CASE("The legacy fixture loads into a game", "[save][legacy][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  GameContext context;
  GameState state;
  std::string error;
  REQUIRE(SaveGameManager::load(state, context,
                                fixture("legacy_v1.plsave").string(), &error));
  CHECK(error.empty());
  REQUIRE(state.active_player() != nullptr);
  CHECK(state.active_player()->name() == "Legacy Pizza");
  CHECK(state.time().time_blocks() == 54);
  CHECK(state.active_city_id() == core::CityId{0});
}

TEST_CASE("A load that is turned away keeps a real game as it was",
          "[save][load][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  TempDir dir;
  GameContext context;
  GameState state;
  start_free_game(state, context);
  const save::StateFingerprint before =
      SaveGameManager::fingerprint(state, context);
  const std::size_t ui_depth = state.ui().size();

  // The checks don't look this deep: a field missing inside a player only
  // shows while building it. Loading straight into the running game would
  // have thrown that game away by then.
  json document = before.document;
  document["saved_at"] = "test";
  document["players"][1].erase("jail_time");
  const fs::path file = dir.path() / "deep-damage.plsave";
  write_file(file, encode(RawSave{.game_data = document.dump()}));

  std::string error;
  CHECK_FALSE(SaveGameManager::load(state, context, file.string(), &error));
  CHECK(error == damaged);
  require_same_game(before, SaveGameManager::fingerprint(state, context));
  CHECK(state.ui().size() == ui_depth);
}
