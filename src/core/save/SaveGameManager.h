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
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/save/SaveVersions.h"
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

// The slot the game keeps a game in progress in before something replaces
// it; see SaveGameManager::checkpoint.
constexpr std::string_view checkpoint_filename = "CHECKPNT.plsave";

// What is about to replace the game in progress.
enum class CheckpointReason { BeforeNewGame, BeforeLoad };

enum class CheckpointResult {
  Written,   // the game is kept as a new, verified checkpoint version
  Unchanged, // the newest checkpoint already holds this very game
  NoGame,    // there is no game in progress to keep
  Failed,    // the game could not be kept; the log says why
};

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
  // The display name of the active location when it is not one of the
  // original ten (a version 4 save); empty otherwise, and the original name
  // comes from the city number.
  std::string active_city_name;
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
  // Writes the save as one file in one step, keeping no versions: a failed
  // save leaves any earlier file of that name as it was. For tools and
  // tests; the game saves with save_to_slot.
  static bool save(const GameState& state, const GameContext& context,
                   const std::string& filename, std::string* error = nullptr);

  // Saves to a slot and keeps its versions (SaveVersions.h). The new save is
  // written under a name of its own, then integrity-checked (checksum and
  // header) and load-verified (built apart from the game, it has the saved
  // game's fingerprint). Only then does it become a working version and the
  // slot's file, and the oldest working version past three goes. A save
  // that fails a check stays a candidate, with the reason, and every working
  // version stays as it was. Returns false, with a message for the player,
  // unless the save became a working version.
  static bool save_to_slot(const GameState& state, const GameContext& context,
                           const std::string& filename,
                           std::string* error = nullptr);
  static bool autosave(const GameState& state, const GameContext& context,
                       std::string* error = nullptr);

  // Checks the whole save before the running game is touched. A save that
  // fails any check is rejected and the game carries on exactly as it was.
  // A slot with versions loads its active version, and if that can't be
  // loaded, its other working versions, newest first; then notice, when
  // given, and the status line say which was loaded.
  static bool load(GameState& state, GameContext& context,
                   const std::string& filename, std::string* error = nullptr,
                   std::string* notice = nullptr);

  // As load, for the player's Load over a game in progress: once the save
  // has passed every check, and just before it replaces that game, keeps the
  // game as a checkpoint. The save is read before the checkpoint is written,
  // so loading the checkpoint slot itself loads what it held when chosen. A
  // failed checkpoint does not stop the load; notice says so. The
  // checkpoint goes to checkpoint_slot, or to checkpoint_filename in the
  // saves folder if that is empty.
  static bool
  load_over_current(GameState& state, GameContext& context,
                    const std::string& filename, std::string* error = nullptr,
                    std::string* notice = nullptr,
                    const std::filesystem::path& checkpoint_slot = {});

  // The hook the game calls before something replaces the game in progress:
  // keeps that game as a new version of the checkpoint slot. Nothing is
  // written when there is no game, or when the newest checkpoint holds this
  // very game (equal fingerprints). The slot is checkpoint_filename in the
  // saves folder unless another is given.
  static CheckpointResult
  checkpoint(const GameState& state, const GameContext& context,
             CheckpointReason reason,
             const std::filesystem::path& slot_file = {});

  // A slot's versions, or nothing if it keeps none; problem, when given,
  // says why a manifest that is there could not be read.
  static std::optional<SlotManifest> versions(const std::string& filename,
                                              std::string* problem = nullptr);

  // Makes a working version of a slot the one it loads, after checking it
  // again apart from the game. On any failure the slot keeps the version it
  // had; a version that fails the check becomes a candidate, with the reason.
  static bool restore_version(const GameState& state,
                              const GameContext& context,
                              const std::string& filename,
                              std::uint64_t sequence,
                              std::string* error = nullptr);

  // Deletes a save: its file and every version of it.
  static bool delete_save(const std::string& filename,
                          std::string* error = nullptr);

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
                            const GameContext& context);

  // Builds the save in these bytes apart from the game, as like's options
  // would, and checks it has the expected fingerprint. Returns why not.
  static std::optional<std::string>
  verify_build(std::string_view bytes, const std::string& name,
               const std::string& expected_fingerprint, const GameState& like,
               const GameContext& context);

  // One save to a slot; see save_to_slot.
  static bool save_version(const GameState& state, const GameContext& context,
                           const std::filesystem::path& path,
                           std::string_view made_for, std::string* error);

  // load and load_over_current. before_replacing runs once, after the save
  // passed every check and before it replaces the game.
  static bool load_slot(GameState& state, GameContext& context,
                        const std::string& filename, std::string* error,
                        std::string* notice,
                        const std::function<void()>& before_replacing);

  static std::string get_full_path(const std::string& filename);
};

} // namespace save
