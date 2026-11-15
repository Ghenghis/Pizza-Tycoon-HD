/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/save/CheckpointTimer.h"
#include "core/save/SaveVersions.h"
#include "core/save/Session.h"
#include "core/util/Constants.h"

class GameState;
class GameContext;
struct GameOptions;

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

// The slot timed checkpoints go to. Only TimedCheckpoints writes it, so a
// worker never meets another writer there.
constexpr std::string_view timed_checkpoint_filename = "AUTOCHK.plsave";

// A timed checkpoint that passed every check, the build included, and waits
// for the game thread to make it a working version.
constexpr std::string_view awaiting_promotion = "awaiting promotion";

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

// What the main menu offers after a run that did not end cleanly.
struct ResumeOffer {
  std::string slot_file; // the timed checkpoint slot, as load_version takes it
  std::uint64_t sequence = 0;
  int day = 0;           // the game's day, from 1
  std::string time;      // its time of day, "14:30"
  std::string game_date; // as the load menu shows it
};

// One save slot as the Recovery Center shows it.
struct RecoverySlot {
  std::string name;        // the slot's file name, as "MYGAME.plsave"
  bool checkpoint = false; // a slot the game writes on its own
  bool has_file = false;   // whether the slot's own file is there
  std::optional<SlotManifest> versions; // nothing if it keeps none
  std::string problem; // why a manifest that is there could not be read
};

// A save written and checked, the build included, off the game thread: a
// timed checkpoint or an autosave. The game thread only captured the game.
class CheckpointJob {
public:
  CheckpointJob() = default;
  ~CheckpointJob() = default; // the jthread joins
  CheckpointJob(const CheckpointJob&) = delete;
  CheckpointJob& operator=(const CheckpointJob&) = delete;

  [[nodiscard]] bool done() const { return shared_->done.load(); }
  void wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  // The slot it writes.
  [[nodiscard]] const std::filesystem::path& slot_file() const {
    return slot_file_;
  }
  // Once done: whether the new version passed every check and waits to be
  // promoted, whether there was nothing new to keep, which version it is,
  // and if it failed, why.
  [[nodiscard]] bool ready() const { return shared_->ready; }
  [[nodiscard]] bool unchanged() const { return shared_->unchanged; }
  [[nodiscard]] std::uint64_t sequence() const { return shared_->sequence; }
  [[nodiscard]] const std::string& error() const { return shared_->error; }

  // What the worker hands back. It writes the rest before done, and the
  // game thread reads them only after it sees done.
  struct Shared {
    std::atomic<bool> done{false};
    bool ready = false;
    bool unchanged = false;
    std::uint64_t sequence = 0;
    std::string error;
  };

private:
  friend class SaveGameManager;
  std::filesystem::path slot_file_;
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
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
  // The autosave, all on this thread: as save_to_slot, to the next
  // AUTOSAVn slot.
  static bool autosave(const GameState& state, const GameContext& context,
                       std::string* error = nullptr);

  // The autosave the game makes mid-day: the game is captured now, on this
  // thread; the rest runs on a worker, and settle_background_saves promotes
  // it on the game thread. Returns false, having started nothing, only if
  // the game could not be captured.
  // The slot is the next AUTOSAVn in folder, the saves folder if empty.
  static bool autosave_in_background(const GameState& state,
                                     const GameContext& context,
                                     const std::filesystem::path& folder = {});

  // On the game thread, every frame: promotes the background autosaves
  // that are done, and tells the player of one that failed, as the autosave
  // always has.
  static void settle_background_saves(GameState& state);

  // Waits for every background autosave and promotes those that passed, as
  // the game closes. Nobody is told; the log has it.
  static void finish_background_saves();

  // How many background autosaves are running or not yet promoted.
  static std::size_t background_saves();

  // What a save takes on the game thread: the game as save JSON, captured at
  // one moment. Everything else can run on a worker.
  static json capture(const GameState& state, const GameContext& context);

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

  // As load_over_current, for one working version of a slot (the Recovery
  // Center's Load this version): that version alone is tried, checked
  // against its manifest entry first.
  static bool load_version(GameState& state, GameContext& context,
                           const std::string& filename, std::uint64_t sequence,
                           std::string* error = nullptr,
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

  // Starts a timed checkpoint. The game is captured now, on this thread.
  // The worker fingerprints it, and if the newest checkpoint (working or
  // waiting) holds the same game it stops there (the job's unchanged());
  // otherwise it writes the version and runs every check, the build apart
  // from the game included. Returns no job, with result set to NoGame, when
  // there is no game; result is Written when a job started, and Failed if
  // the game could not be captured.
  static std::unique_ptr<CheckpointJob>
  start_timed_checkpoint(const GameState& state, const GameContext& context,
                         TimedCheckpoint reason,
                         CheckpointResult* result = nullptr,
                         const std::filesystem::path& slot_file = {});

  // On the game thread, once the job is done: makes its version a working,
  // active one, in one small manifest write; the worker already wrote the
  // slot's file. Written when it did, Unchanged while the job runs or when
  // there was nothing new, Failed if the version did not pass its checks.
  static CheckpointResult promote(const CheckpointJob& job);

  // The Recovery Center's model: every slot in the folder (the saves folder
  // if empty), with its versions and their levels, by name.
  static std::vector<RecoverySlot>
  recovery_slots(const std::filesystem::path& folder = {});

  // Copies a working version into a new slot, after checking it again apart
  // from the game. The slot it came from is not touched. Refuses a new name
  // that is already a save or has versions.
  static bool restore_as_branch(const GameState& state,
                                const GameContext& context,
                                const std::string& filename,
                                std::uint64_t sequence,
                                const std::string& new_filename,
                                std::string* error = nullptr);

  // Undoes the last restore_version of a slot: the version active before it
  // becomes active again, after the same checks.
  static bool undo_restore(const GameState& state, const GameContext& context,
                           const std::string& filename,
                           std::string* error = nullptr);

  // At start-up, once the saves folder is known (the saves folder if folder
  // is empty): reads what the last run left, works out whether to offer
  // resuming it (choose_resume), and records this run as begun and not yet
  // ended cleanly. Manual saves from here on are noted in the record.
  static void begin_session(const std::filesystem::path& folder = {});
  // When the game closes cleanly, after the background saves are done.
  static void end_session();

  // The offer worked out by begin_session, unchecked; for diagnostics and
  // tests.
  static std::optional<ResumeCandidate> pending_resume();

  // The main menu asks once: the version to offer, checked again apart from
  // the game (integrity and load check) and with its game day and time.
  // Nothing if there is none or it fails its checks; asking again gives
  // nothing either. Resuming is load_version on it; nothing loads by itself
  // and nothing is deleted.
  static std::optional<ResumeOffer>
  take_resume_offer(const GameState& like, const GameContext& context);

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

  // Builds the save in these bytes apart from the game, with these options,
  // and checks it has the expected fingerprint. Returns why not. Safe on a
  // worker: its random streams and the factories' tables are per thread or
  // read-only there.
  static std::optional<std::string>
  verify_build(std::string_view bytes, const std::string& name,
               const std::string& expected_fingerprint,
               const GameOptions& options, const GameContext& context);

  // Starts the worker of a background save of this captured game.
  static std::unique_ptr<CheckpointJob>
  start_background(const std::filesystem::path& path, json document,
                   std::string_view made_for, const GameOptions& options,
                   bool coalesce);

  // One save to a slot; see save_to_slot.
  static bool save_version(const GameState& state, const GameContext& context,
                           const std::filesystem::path& path,
                           std::string_view made_for, std::string* error);

  // load and load_over_current. before_replacing runs once, after the save
  // passed every check and before it replaces the game.
  // With only_version, that working version alone is tried.
  static bool load_slot(GameState& state, GameContext& context,
                        const std::string& filename, std::string* error,
                        std::string* notice,
                        const std::function<void()>& before_replacing,
                        std::optional<std::uint64_t> only_version = {});

  static std::string get_full_path(const std::string& filename);
};

// The game's timed checkpoints: give it every frame. It starts a checkpoint
// every five minutes of play and when a game day or week begins, written and
// checked on a worker, and promotes it on the game thread once it is done.
class TimedCheckpoints {
public:
  explicit TimedCheckpoints(std::filesystem::path slot_file = {});

  // playing: the game ran this frame, not paused or in a menu. Also settles
  // the background autosaves (SaveGameManager::settle_background_saves).
  void frame(GameState& state, const GameContext& context,
             std::chrono::milliseconds time, bool playing);

  // Waits for a checkpoint or autosave still being written, as the game
  // closes, and promotes it.
  void finish();

private:
  std::filesystem::path slot_file_;
  CheckpointTimer timer_;
  std::unique_ptr<CheckpointJob> job_;
};

} // namespace save
