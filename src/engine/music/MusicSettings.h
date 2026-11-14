/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/music/JsonStore.h"
#include "engine/music/MusicLibrary.h"
#include "engine/music/MusicTypes.h"

namespace music {

struct Playlist {
  PlaylistId id;
  std::string name; // UTF-8, at most MAX_NAME_BYTES
  // In the player's order. Ids the library does not know are kept, and
  // shown as missing.
  std::vector<TrackId> tracks;
  std::uint32_t revision = 0; // +1 on every save; a stale save is refused
};

// A collection the player renamed or hid.
struct CollectionPrefs {
  CollectionId id;
  std::string name; // empty: the folder's name
  bool hidden = false;
};

// Everything the music menus set up once and keep: profile-local, never in
// a game save (a save may later name a playlist by id, nothing more). The
// music volume stays in the game's config with the other volumes.
struct MusicSettings {
  Mode mode = Mode::ORIGINAL;
  std::vector<FolderChoice> folders;
  // Unset: the game's Xmas folder is used only while no folder is chosen.
  std::optional<bool> use_game_folder;
  std::vector<CollectionPrefs> collections;
  std::vector<Playlist> playlists;
  PlaylistId playlist; // queued; empty for the collections below
  std::vector<CollectionId> queue_collections;
  // Where the queue was.
  TrackId resume_track;
  double resume_seconds = 0.0;
  Repeat repeat = Repeat::ALL;
  bool shuffle = false;
  bool collapsed = true; // the mini-player
  // MusicPlayback::Policy
  bool autostart_north_pole = false;
  bool keep_everywhere = false;
  bool background_after_jukebox = false;
  Mode north_pole_mode = Mode::CHRISTMAS_BACKGROUND;
  Backend backend = Backend::AUTO;
};

struct SaveResult {
  std::string error; // empty on success
  std::uint32_t revision = 0;
  explicit operator bool() const { return error.empty(); }
};

// One entry of a playlist as shown: the track, or missing.
struct PlaylistEntry {
  TrackId id;
  std::optional<TrackInfo> track;
  bool missing = true;
};
[[nodiscard]] std::vector<PlaylistEntry>
playlist_entries(const Playlist& playlist, const MusicLibrary& library);

// music-settings.json, kept as a RevisionedDocument: each save is checked
// before it replaces the last one, and three working copies stay behind.
// Game thread only.
class SettingsStore {
public:
  static constexpr int SCHEMA = 1;
  static constexpr std::size_t MAX_PLAYLISTS = 1000;
  static constexpr std::size_t MAX_PLAYLIST_TRACKS = 10000;
  static constexpr std::size_t MAX_FOLDERS = 32;

  explicit SettingsStore(std::filesystem::path file);

  // Reads the newest working copy. With none at all, the defaults (a first
  // start). False when copies exist but none works: the defaults are used,
  // the copies stay on disk, and problems says why.
  bool load(std::vector<std::string>* problems = nullptr);
  // Which copy load() used: "current", "candidate", "backup 1" ... or
  // empty for the defaults.
  [[nodiscard]] const std::string& loaded_from() const { return source_; }

  [[nodiscard]] const MusicSettings& settings() const { return settings_; }
  // Saves every setting but the playlists, which keep their own revisions.
  SaveResult save_settings(const MusicSettings& settings);

  [[nodiscard]] std::optional<Playlist> playlist(const PlaylistId& id) const;
  // A new, empty playlist with a fresh pl: id; empty when refused.
  PlaylistId create_playlist(std::string name, std::string* error = nullptr);
  // Creates or replaces one playlist. Refused, with the reason, when its
  // revision is not the stored one (it changed since it was read), its
  // name is empty or an id is malformed.
  SaveResult save_playlist(const Playlist& playlist);
  bool remove_playlist(const PlaylistId& id);

  [[nodiscard]] const RevisionedDocument& document() const { return doc_; }

private:
  SaveResult write(const MusicSettings& next);

  RevisionedDocument doc_;
  MusicSettings settings_;
  std::string source_;
};

} // namespace music
