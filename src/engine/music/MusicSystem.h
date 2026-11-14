/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine/music/MusicLibrary.h"
#include "engine/music/MusicPlayback.h"
#include "engine/music/MusicSettings.h"

namespace audio {
class AudioManager;
}

namespace music {

// The folder list as the Music folders screen edits it. Nothing changes for
// the game until MusicSystem::save_folders(); cancel() goes back to the saved
// list, reset() to the suggested folders (saved only by save_folders()).
class FolderDraft {
public:
  [[nodiscard]] const std::vector<FolderChoice>& folders() const {
    return draft_;
  }
  [[nodiscard]] bool dirty() const { return draft_ != saved_; }
  // Whether the saved list has this entry, so a scan has looked at it.
  [[nodiscard]] bool saved(const std::string& id) const {
    return std::any_of(saved_.begin(), saved_.end(),
                       [&](const FolderChoice& f) { return f.id == id; });
  }

  // Adds a folder of any name and returns its new id; empty, with the reason,
  // when it is not a folder or is in the list already.
  std::string add(const std::filesystem::path& folder,
                  std::string* error = nullptr);
  bool remove(const std::string& id);
  // The label shown in the game; the folder itself is never renamed.
  bool relabel(const std::string& id, std::string label);
  bool set_enabled(const std::string& id, bool enabled);
  bool set_include_subfolders(const std::string& id, bool include);
  // Points the entry at its folder's new place. It keeps its id, so its
  // collections keep theirs, with their labels, and playlists play on.
  bool relink(const std::string& id, const std::filesystem::path& folder,
              std::string* error = nullptr);
  void cancel() { draft_ = saved_; }
  void reset() { draft_ = defaults_; }

private:
  friend class MusicSystem;
  FolderChoice* find(const std::string& id);

  std::vector<FolderChoice> draft_;
  std::vector<FolderChoice> saved_;
  std::vector<FolderChoice> defaults_;
  std::filesystem::path game_dir_;
};

// The player's music as the game holds it: the settings, the folder library
// and the one music owner, made and destroyed in that order around them.
// Destroy it before the AudioManager and before SDL_Quit(). Game thread.
class MusicSystem {
public:
  struct Paths {
    std::filesystem::path profile;    // platform_paths::music_dir()
    std::filesystem::path game_dir;   // platform_paths::game_dir()
    std::filesystem::path user_music; // the system's Music folder, or empty
  };

  MusicSystem(audio::AudioManager& audio, Paths paths);
  // Saves the mode and where the queue was.
  ~MusicSystem();
  MusicSystem(const MusicSystem&) = delete;
  MusicSystem& operator=(const MusicSystem&) = delete;

  [[nodiscard]] MusicPlayback& playback() { return *playback_; }
  [[nodiscard]] MusicLibrary& library() { return *library_; }
  [[nodiscard]] SettingsStore& store() { return *store_; }
  // Why the settings could not be read as they were, if so.
  [[nodiscard]] const std::vector<std::string>& problems() const {
    return problems_;
  }

  // The player's choice, saved.
  void set_mode(Mode mode);
  [[nodiscard]] Mode mode() const { return playback_->mode(); }
  // Restore original audio: Original music at once, from where the FM song
  // was, while the game goes on. Folders and playlists stay; the effects are
  // the AudioManager's (select_effects()).
  void restore_original();
  // The player's choices in the mini-player, saved.
  void set_repeat(Repeat repeat);
  void set_shuffle(bool shuffle);
  [[nodiscard]] bool bar_collapsed() const {
    return store_->settings().bar_collapsed;
  }
  void set_bar_collapsed(bool collapsed);

  // The folder screen.
  [[nodiscard]] FolderDraft& folders();
  struct FolderRow {
    FolderChoice folder;
    std::filesystem::path resolved;
    bool available = false; // the folder is there
    std::size_t tracks = 0; // as of the last finished scan
  };
  [[nodiscard]] std::vector<FolderRow> folder_rows() const;
  // Saves the draft, then scans; the song playing plays on. On failure the
  // last working list stays in use and the draft stays as it is.
  SaveResult save_folders();

  // The collections with the player's labels, hidden ones left out unless
  // asked for.
  [[nodiscard]] std::vector<Collection>
  collections(bool with_hidden = false) const;
  SaveResult rename_collection(const CollectionId& id, std::string label);
  SaveResult hide_collection(const CollectionId& id, bool hidden);
  // Locate: every playlist naming old_id names new_id instead, in the same
  // place. new_id must be a track of the library.
  SaveResult relink_track(const TrackId& old_id, const TrackId& new_id);

  // Opens the system's folder picker. What the player picks is added to the
  // folder draft (relink_id empty) or relinks that entry, at the next
  // tick(); folder_message() then says how it went. The picker answers on
  // its own time and maybe another thread; this object may be gone by then.
  void browse_folder(const std::string& relink_id = {});
  [[nodiscard]] const std::string& folder_message() const {
    return folder_message_;
  }
  // The newest notice of the music owner ("Playing ...", "Skipped ...").
  [[nodiscard]] const std::string& last_notice() const { return last_notice_; }

  // Game thread, once a frame: takes notices and folder picks, and queues
  // the music when a scan has finished.
  void tick();

private:
  void scan();
  void refresh_queue();
  [[nodiscard]] std::vector<QueuedTrack> wanted_queue() const;
  [[nodiscard]] std::vector<FolderChoice> current_folders() const;
  void save_state();

  audio::AudioManager& audio_;
  Paths paths_;
  std::vector<std::string> problems_;
  std::unique_ptr<SettingsStore> store_;
  std::unique_ptr<MusicLibrary> library_;
  std::unique_ptr<MusicPlayback> playback_;
  FolderDraft draft_;
  // Folders picked in the system dialog: (relink id, path), filled by its
  // callback.
  struct Picks {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> items;
  };
  std::shared_ptr<Picks> picks_ = std::make_shared<Picks>();
  std::string folder_message_;
  std::string last_notice_;
  bool scanned_ = false;
  std::uint32_t scans_seen_ = 0;
};

} // namespace music
