/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/Constants.h"

class GameState;
class GameContext;

namespace save {

using json = nlohmann::json;

// Version of the save format
constexpr int SAVE_FORMAT_VERSION = 3;

// The oldest version a save can have and still be loaded.
constexpr int OLDEST_LOADABLE_SAVE_VERSION = 1;

constexpr std::string_view autosave_prefix = "AUTOSAV";

// Limits a save file has to stay within. The saves the game writes are far
// below them, and save() refuses to write one that is not, so every save it
// writes can be loaded again. They keep a damaged or hostile file from making
// the loader allocate without bound.
constexpr std::uint64_t MAX_SAVE_FILE_SIZE = 64ULL * 1024 * 1024;  // on disk
constexpr std::uint64_t MAX_SAVE_JSON_SIZE = 256ULL * 1024 * 1024; // inflated
constexpr int MAX_SAVE_JSON_DEPTH = 64; // nested objects/arrays
constexpr std::uint64_t MAX_SAVE_JSON_VALUES = 16'000'000; // all JSON values

// A temporary file this old belongs to a write that never finished: no save
// or settings write takes more than a moment.
constexpr std::chrono::hours STALE_TEMP_FILE_AGE{1};

struct SaveMetadata {
  std::string display_name;
  std::string saved_at;
  std::string game_date;
  std::array<std::string, NUMBER_OF_PLAYERS> player_names;
  int active_player = -1;
  int active_city = -1;
};

// Highscore files are basically the same as save games, zlib compressed json
// with a magic header, so for now I don't think it's worth pulling that out
// into its own class.
struct HighScoreEntry {
  std::string name;
  int score;
};

constexpr int HIGH_SCORE_MAX_ENTRIES = 8;

struct StateFingerprint {
  std::string hash; // 16 hex digits
  json document;    // the state we hashed, everything but the save timestamp
};

// Save and load take a file name in the saves folder; an absolute path is
// used as it is. On failure they return false, log the details and, when
// error is given, put a short message for the player in it.
class SaveGameManager {
public:
  // Writes the save in one step: a failed save leaves any earlier file of that
  // name as it was.
  static bool save(const GameState& state, const GameContext& context,
                   const std::string& filename, std::string* error = nullptr);
  static bool autosave(const GameState& state, const GameContext& context,
                       std::string* error = nullptr);

  // Checks the whole save before the running game is touched. A save that
  // fails any check is rejected and the game carries on exactly as it was.
  static bool load(GameState& state, GameContext& context,
                   const std::string& filename, std::string* error = nullptr);

  // Whether the file passes load's checks on its framing, version and
  // structure. Loading can still reject it for damage deeper inside.
  static bool is_valid_save(const std::string& filename);
  static bool is_autosave_filename(const std::string& filename);
  // The save format version, or -1 if the file can't be read.
  static int get_save_version(const std::string& filename);

  // Read metadata from a save file without loading the full game state.
  static std::optional<SaveMetadata>
  read_save_metadata(const std::string& filename);

  static std::vector<HighScoreEntry> load_high_scores();
  static bool add_high_score(HighScoreEntry entry);

  static StateFingerprint fingerprint(const GameState& state,
                                      const GameContext& context);

  // Removes the temporary files that saves, settings and key bindings writes
  // cut short by a crash or a power cut left behind: in the saves folder and
  // the folders of the settings file and the bindings file, older than
  // STALE_TEMP_FILE_AGE. Logs each file removed. Call once at startup, after
  // platform_paths::init(). Returns how many it removed.
  static int remove_stale_temp_files(const std::string& settings_file);
  // The same for these folders, each looked at once.
  static int
  remove_stale_temp_files(const std::vector<std::filesystem::path>& folders,
                          std::chrono::seconds min_age);

private:
  static json state_to_json(const GameState& state, const GameContext& context);
  static void json_to_state(const json& j, GameState& state,
                            GameContext& context);

  static std::string get_full_path(const std::string& filename);
};

} // namespace save
