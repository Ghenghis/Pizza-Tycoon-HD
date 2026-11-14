/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine/music/MusicTypes.h"

namespace music {

// A folder the player's music comes from. Its subfolders are collections.
// The songs are only ever read, never changed, moved or copied.
struct MusicRoot {
  std::string key;            // "game", or "folder:<16 hex>" for a chosen one
  std::filesystem::path path; // absolute
  std::string name;           // the folder's own name, for display
};

// A folder the player chose, as the settings keep it.
struct FolderChoice {
  std::string path; // UTF-8, absolute; device-local
  bool enabled = true;
};

// The "Xmas" folder beside the game's executable (game_dir), when there is
// one.
[[nodiscard]] std::optional<MusicRoot>
game_music_root(const std::filesystem::path& game_dir);
[[nodiscard]] MusicRoot chosen_root(const std::filesystem::path& folder);

// The folders to scan: the chosen ones that are enabled, plus the game's
// Xmas folder when use_game_folder says so. Left unset, the Xmas folder is
// used only while no folder is chosen: a chosen folder takes precedence and
// is never silently merged with it (game_folder_offered() tells the menu to
// let the player pick or combine).
[[nodiscard]] std::vector<MusicRoot>
active_roots(const std::vector<FolderChoice>& chosen,
             std::optional<bool> use_game_folder,
             const std::optional<MusicRoot>& game);
[[nodiscard]] bool game_folder_offered(const std::vector<FolderChoice>& chosen,
                                       std::optional<bool> use_game_folder,
                                       const std::optional<MusicRoot>& game);

// Where a copy of a track is: a root and a path below it.
struct TrackSource {
  std::string root; // MusicRoot::key
  std::filesystem::path relative;
};

struct TrackInfo {
  TrackId id; // from the SHA-256 of the bytes: a moved file keeps it
  std::string sha256;
  std::uint64_t bytes = 0;
  Format format = Format::MP3;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint64_t frames = 0; // at sample_rate
  bool frames_exact = false;
  std::uint32_t bitrate_kbps = 0;
  std::string encoding;
  // From the tags, or the file's name when it has no title.
  std::string title;
  std::string artist;
  std::string album;
  // The first one plays; the rest are files with exactly the same bytes.
  std::vector<TrackSource> sources;
};

// A folder of songs under a root: each subfolder of a root is one, and the
// songs straight in a root make one named after it. Songs in deeper folders
// belong to the collection above them; when they all sit in one album
// folder, the breadcrumb names the way down ("Artist > Album", with U+203A).
struct Collection {
  CollectionId id;
  std::string root; // MusicRoot::key
  std::string name; // the folder's name
  std::string breadcrumb;
  std::filesystem::path folder; // relative to the root
  std::vector<TrackId> tracks;  // by path
  std::uint64_t frames = 0;     // total at 44100 Hz, as far as known
};

// The player's music as found in the folders. The index of track hashes is
// cached (music-library.json), so a file is hashed once and again only when
// its size or time changes. Scans run in small steps on the music worker
// (see run_step()), can be cancelled, and never follow links or junctions,
// so they stay inside the chosen folders.
// What one scan may do.
struct LibraryBounds {
  std::size_t max_files = 10000; // listed per scan
  int max_depth = 8;             // folders below a root
  std::size_t entries_per_step = 256;
  std::size_t bytes_per_step = std::size_t{1} << 20; // hashing
  std::size_t max_problems = 200;
  Limits limits;
};

class MusicLibrary {
public:
  using Bounds = LibraryBounds;

  struct Progress {
    bool scanning = false;
    std::size_t files_found = 0;
    std::size_t files_done = 0;
    std::size_t tracks = 0;
    std::size_t collections = 0;
    std::size_t refused = 0;
    std::uint32_t scans_finished = 0;
  };

  // index_file: the cache of hashes, in the profile's music folder.
  explicit MusicLibrary(std::filesystem::path index_file, Bounds bounds = {});
  ~MusicLibrary();
  MusicLibrary(const MusicLibrary&) = delete;
  MusicLibrary& operator=(const MusicLibrary&) = delete;

  // Game thread. Reads the cache. An absent or damaged cache only means
  // more hashing; the reason goes to *error.
  bool load_cache(std::string* error = nullptr);
  // Sets the folders and scans them again from the start.
  void set_roots(std::vector<MusicRoot> roots);
  void rescan();
  // Stops the scan; the tracks of the last finished scan stay.
  void cancel_scan();

  [[nodiscard]] Progress progress() const;
  [[nodiscard]] std::vector<MusicRoot> roots() const;
  [[nodiscard]] std::vector<Collection> collections() const;
  [[nodiscard]] std::vector<TrackInfo> tracks() const;
  [[nodiscard]] std::optional<TrackInfo> find(const TrackId& id) const;
  // The file to play: the first source under a current root; empty when
  // there is none.
  [[nodiscard]] std::filesystem::path file_of(const TrackInfo& track) const;
  // Files the last scans refused or could not read, each with the reason,
  // since the last call.
  [[nodiscard]] std::vector<std::string> poll_problems();

  // The worker that runs the scans calls this with its wake function, and
  // with an empty one when it stops.
  void attach_worker(std::function<void()> wake);
  // Worker: one bounded step of the scan, if one is due. True while more
  // work waits.
  bool run_step();

private:
  struct Scan;

  void request_scan();
  void problem(std::string text);
  void finish_scan();
  void write_cache();

  std::filesystem::path index_file_;
  Bounds bounds_;

  mutable std::mutex mutex_;
  std::vector<MusicRoot> roots_;
  std::map<TrackId, TrackInfo> tracks_;
  std::vector<Collection> collections_;
  // The cache: path key ("<root key>/<relative>") to what was found there.
  struct Cached {
    std::uint64_t bytes = 0;
    std::int64_t time = 0;
    TrackInfo track; // sources left empty
  };
  std::map<std::string, Cached> cache_;
  std::vector<std::string> problems_;
  std::size_t problems_dropped_ = 0;
  bool scan_requested_ = false;
  Progress progress_;
  std::function<void()> wake_;

  std::atomic<bool> cancel_{false};
  // Worker only.
  std::unique_ptr<Scan> scan_;
};

} // namespace music
