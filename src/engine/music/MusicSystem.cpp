/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/MusicSystem.h"

#include <algorithm>
#include <functional>
#include <set>
#include <system_error>

#include "engine/Logger.h"
#include "engine/SDL_include.h"
#include "engine/audio/AudioManager.h"

namespace fs = std::filesystem;

namespace music {

namespace {

bool same_folder(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec)) {
    return true;
  }
  return a.lexically_normal() == b.lexically_normal();
}

bool fail(std::string* error, const char* why) {
  if (error) {
    *error = why;
  }
  return false;
}

} // namespace

/* FolderDraft */

FolderChoice* FolderDraft::find(const std::string& id) {
  const auto it =
      std::find_if(draft_.begin(), draft_.end(),
                   [&](const FolderChoice& f) { return f.id == id; });
  return it == draft_.end() ? nullptr : &*it;
}

std::string FolderDraft::add(const fs::path& folder, std::string* error) {
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) {
    fail(error, "that is not a folder");
    return {};
  }
  if (draft_.size() >= SettingsStore::MAX_FOLDERS) {
    fail(error, "no more folders can be added");
    return {};
  }
  for (const FolderChoice& f : draft_) {
    if (same_folder(resolve_path(f.path, game_dir_), folder)) {
      fail(error, "that folder is in the list already");
      return {};
    }
  }
  FolderChoice choice = new_folder(folder, game_dir_);
  draft_.push_back(choice);
  return choice.id;
}

bool FolderDraft::remove(const std::string& id) {
  const auto before = draft_.size();
  std::erase_if(draft_, [&](const FolderChoice& f) { return f.id == id; });
  return draft_.size() != before;
}

bool FolderDraft::relabel(const std::string& id, std::string label) {
  FolderChoice* f = find(id);
  if (!f) {
    return false;
  }
  f->label = clean_text(label, MAX_NAME_BYTES);
  return true;
}

bool FolderDraft::set_enabled(const std::string& id, bool enabled) {
  FolderChoice* f = find(id);
  if (f) {
    f->enabled = enabled;
  }
  return f != nullptr;
}

bool FolderDraft::set_include_subfolders(const std::string& id, bool include) {
  FolderChoice* f = find(id);
  if (f) {
    f->include_subfolders = include;
  }
  return f != nullptr;
}

bool FolderDraft::relink(const std::string& id, const fs::path& folder,
                         std::string* error) {
  FolderChoice* f = find(id);
  if (!f) {
    return fail(error, "that folder is not in the list");
  }
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) {
    return fail(error, "that is not a folder");
  }
  for (const FolderChoice& other : draft_) {
    if (other.id != id &&
        same_folder(resolve_path(other.path, game_dir_), folder)) {
      return fail(error, "that folder is in the list already");
    }
  }
  f->path = stored_path(folder, game_dir_);
  return true;
}

/* MusicSystem */

MusicSystem::MusicSystem(audio::AudioManager& audio, Paths paths)
    : audio_(audio), paths_(std::move(paths)),
      store_(std::make_unique<SettingsStore>(paths_.profile /
                                             "music-settings.json")),
      library_(std::make_unique<MusicLibrary>(paths_.profile /
                                              "music-library.json")) {
  if (!store_->load(&problems_) || store_->locked()) {
    for (const std::string& p : problems_) {
      Logger::warn("Music settings: {}", p);
    }
  }
  if (!store_->loaded_from().empty() && store_->loaded_from() != "current") {
    Logger::warn("Music settings: the {} copy was used", store_->loaded_from());
  }
  std::string cache_error;
  if (!library_->load_cache(&cache_error) && !cache_error.empty()) {
    Logger::info("Music cache: {}; the folders are hashed again", cache_error);
  }
  playback_ = std::make_unique<MusicPlayback>(audio_, library_.get());

  const MusicSettings& s = store_->settings();
  draft_.game_dir_ = paths_.game_dir;
  draft_.defaults_ = default_folders(paths_.user_music, paths_.game_dir);
  draft_.saved_ = current_folders();
  draft_.draft_ = draft_.saved_;

  playback_->set_backend(s.backend);
  playback_->set_repeat(s.repeat);
  playback_->set_shuffle(s.shuffle);
  playback_->set_mode(s.mode);
  playback_->set_policy(MusicPlayback::Policy{
      .autostart_north_pole = s.autostart_north_pole,
      .keep_everywhere = s.keep_everywhere,
      .background_after_jukebox = s.background_after_jukebox,
      .north_pole_mode = s.north_pole_mode});
  // Scanning waits until the player's music is wanted, unless the player
  // set folders up before.
  if (s.folders_saved || s.mode == Mode::CHRISTMAS_BACKGROUND ||
      s.mode == Mode::JUKEBOX) {
    scan();
  }
}

MusicSystem::~MusicSystem() {
  save_state();
  // The owner goes first; then the library it scans with; then the store.
  playback_.reset();
  library_.reset();
}

std::vector<FolderChoice> MusicSystem::current_folders() const {
  const MusicSettings& s = store_->settings();
  return s.folders_saved ? s.folders
                         : default_folders(paths_.user_music, paths_.game_dir);
}

void MusicSystem::scan() {
  scanned_ = true;
  library_->set_roots(active_roots(current_folders(), paths_.game_dir));
}

void MusicSystem::save_state() {
  if (store_->locked()) {
    return;
  }
  MusicSettings s = store_->settings();
  s.mode = playback_->mode();
  const MusicPlayback::Place at = playback_->place();
  if (!at.track.empty()) {
    s.last_track = at.track;
    s.resume_seconds = at.position_s;
  }
  s.repeat = playback_->repeat();
  s.shuffle = playback_->shuffle();
  const MusicPlayback::Policy& p = playback_->policy();
  s.autostart_north_pole = p.autostart_north_pole;
  s.keep_everywhere = p.keep_everywhere;
  s.background_after_jukebox = p.background_after_jukebox;
  s.north_pole_mode = p.north_pole_mode;
  if (const auto saved = store_->save_settings(s); !saved) {
    Logger::warn("Music settings not saved: {}", saved.error);
  }
}

void MusicSystem::set_mode(Mode mode) {
  playback_->set_mode(mode);
  if ((mode == Mode::CHRISTMAS_BACKGROUND || mode == Mode::JUKEBOX) &&
      !scanned_) {
    scan();
  }
  save_state();
}

void MusicSystem::restore_original() { set_mode(Mode::ORIGINAL); }

void MusicSystem::set_repeat(Repeat repeat) {
  playback_->set_repeat(repeat);
  save_state();
}

void MusicSystem::set_shuffle(bool shuffle) {
  playback_->set_shuffle(shuffle);
  save_state();
}

void MusicSystem::set_bar_collapsed(bool collapsed) {
  if (store_->locked() || store_->settings().bar_collapsed == collapsed) {
    return;
  }
  MusicSettings s = store_->settings();
  s.bar_collapsed = collapsed;
  if (const auto saved = store_->save_settings(s); !saved) {
    Logger::warn("Music settings not saved: {}", saved.error);
  }
}

FolderDraft& MusicSystem::folders() {
  if (!scanned_) {
    scan(); // the rows show what each folder holds
  }
  return draft_;
}

std::vector<MusicSystem::FolderRow> MusicSystem::folder_rows() const {
  const std::vector<Collection> all = library_->collections();
  std::vector<FolderRow> rows;
  for (const FolderChoice& f : draft_.draft_) {
    FolderRow row{.folder = f,
                  .resolved = resolve_path(f.path, paths_.game_dir),
                  .available = false,
                  .tracks = 0};
    std::error_code ec;
    row.available = fs::is_directory(row.resolved, ec);
    std::set<TrackId> ids;
    for (const Collection& c : all) {
      if (c.root == f.id) {
        ids.insert(c.tracks.begin(), c.tracks.end());
      }
    }
    row.tracks = ids.size();
    rows.push_back(std::move(row));
  }
  return rows;
}

SaveResult MusicSystem::save_folders() {
  MusicSettings s = store_->settings();
  s.folders = draft_.draft_;
  s.folders_saved = true;
  SaveResult saved = store_->save_settings(s);
  if (!saved) {
    return saved;
  }
  draft_.saved_ = draft_.draft_;
  scan();
  return saved;
}

std::vector<Collection> MusicSystem::collections(bool with_hidden) const {
  std::vector<Collection> out;
  const auto& prefs = store_->settings().collections;
  for (Collection c : library_->collections()) {
    const auto p =
        std::find_if(prefs.begin(), prefs.end(),
                     [&](const CollectionPrefs& x) { return x.id == c.id; });
    if (p != prefs.end()) {
      if (p->hidden && !with_hidden) {
        continue;
      }
      if (!p->name.empty()) {
        c.name = p->name;
      }
    }
    out.push_back(std::move(c));
  }
  return out;
}

SaveResult MusicSystem::rename_collection(const CollectionId& id,
                                          std::string label) {
  if (!is_key_of(id, "col")) {
    return SaveResult{.error = "not a collection id", .revision = 0};
  }
  MusicSettings s = store_->settings();
  auto it = std::find_if(s.collections.begin(), s.collections.end(),
                         [&](const CollectionPrefs& p) { return p.id == id; });
  if (it == s.collections.end()) {
    s.collections.push_back(
        CollectionPrefs{.id = id, .name = {}, .hidden = false});
    it = s.collections.end() - 1;
  }
  it->name = clean_text(label, MAX_NAME_BYTES);
  return store_->save_settings(s);
}

SaveResult MusicSystem::hide_collection(const CollectionId& id, bool hidden) {
  if (!is_key_of(id, "col")) {
    return SaveResult{.error = "not a collection id", .revision = 0};
  }
  MusicSettings s = store_->settings();
  auto it = std::find_if(s.collections.begin(), s.collections.end(),
                         [&](const CollectionPrefs& p) { return p.id == id; });
  if (it == s.collections.end()) {
    s.collections.push_back(
        CollectionPrefs{.id = id, .name = {}, .hidden = false});
    it = s.collections.end() - 1;
  }
  it->hidden = hidden;
  const SaveResult saved = store_->save_settings(s);
  if (saved) {
    refresh_queue();
  }
  return saved;
}

SaveResult MusicSystem::relink_track(const TrackId& old_id,
                                     const TrackId& new_id) {
  if (!library_->find(new_id)) {
    return SaveResult{.error = "that track is not in the music folders",
                      .revision = 0};
  }
  for (const Playlist& listed : store_->settings().playlists) {
    if (std::find(listed.tracks.begin(), listed.tracks.end(), old_id) ==
        listed.tracks.end()) {
      continue;
    }
    Playlist p = listed;
    std::replace(p.tracks.begin(), p.tracks.end(), old_id, new_id);
    if (SaveResult saved = store_->save_playlist(p); !saved) {
      return saved;
    }
  }
  refresh_queue();
  return {};
}

std::vector<QueuedTrack> MusicSystem::wanted_queue() const {
  const MusicSettings& s = store_->settings();
  std::vector<TrackId> ids;
  if (const auto list = store_->playlist(s.playlist)) {
    ids = list->tracks;
  }
  else {
    std::set<TrackId> seen;
    for (const Collection& c : collections()) {
      if (!s.queue_collections.empty() &&
          std::find(s.queue_collections.begin(), s.queue_collections.end(),
                    c.id) == s.queue_collections.end()) {
        continue;
      }
      for (const TrackId& id : c.tracks) {
        if (seen.insert(id).second) {
          ids.push_back(id);
        }
      }
    }
  }
  std::vector<QueuedTrack> out;
  out.reserve(ids.size());
  for (const TrackId& id : ids) {
    QueuedTrack t{.id = id, .file = {}, .title = {}};
    if (const auto info = library_->find(id)) {
      t.title = info->title;
      t.file = library_->file_of(*info);
    }
    out.push_back(std::move(t));
  }
  return out;
}

void MusicSystem::refresh_queue() {
  std::vector<QueuedTrack> tracks = wanted_queue();
  if (playback_->queue().empty()) {
    const MusicSettings& s = store_->settings();
    std::size_t first = 0;
    double at = 0.0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].id == s.last_track) {
        first = i;
        at = s.resume_seconds;
        break;
      }
    }
    playback_->set_queue(std::move(tracks), first, at);
  }
  else {
    playback_->edit_queue(std::move(tracks));
  }
}

namespace {

// What travels through the folder dialog: where to put the answer.
struct PickRequest {
  std::weak_ptr<void> owner;
  std::function<void(std::vector<std::string>)> deliver;
};

void SDLCALL on_folders_picked(void* userdata, const char* const* files,
                               int /*filter*/) {
  std::unique_ptr<PickRequest> request(static_cast<PickRequest*>(userdata));
  // The list is only borrowed until this returns: copied now.
  std::vector<std::string> picked;
  for (int i = 0; files && files[i] && i < 32; ++i) {
    picked.emplace_back(files[i]);
  }
  if (!request->owner.expired() && !picked.empty()) {
    request->deliver(std::move(picked));
  }
}

} // namespace

void MusicSystem::browse_folder(const std::string& relink_id) {
  std::weak_ptr<Picks> picks = picks_;
  auto request = std::make_unique<PickRequest>();
  request->owner = picks_;
  request->deliver = [picks, relink_id](std::vector<std::string> folders) {
    if (const auto p = picks.lock()) {
      const std::lock_guard lock(p->mutex);
      for (std::string& f : folders) {
        p->items.emplace_back(relink_id, std::move(f));
      }
    }
  };
  const std::string start = utf8_path(paths_.user_music);
  SDL_ShowOpenFolderDialog(on_folders_picked, request.release(), nullptr,
                           start.empty() ? nullptr : start.c_str(),
                           relink_id.empty());
}

void MusicSystem::tick() {
  std::vector<std::pair<std::string, std::string>> picked;
  {
    const std::lock_guard lock(picks_->mutex);
    picked.swap(picks_->items);
  }
  for (const auto& [relink, path] : picked) {
    std::string error;
    const fs::path folder = path_from_utf8(path);
    const std::string name = utf8_path(folder.filename());
    if (relink.empty()) {
      folder_message_ = draft_.add(folder, &error).empty()
                            ? name + ": " + error
                            : "Added " + name + "; Save keeps it";
    }
    else {
      folder_message_ = draft_.relink(relink, folder, &error)
                            ? "Now at " + name + "; Save keeps it"
                            : name + ": " + error;
    }
  }
  for (const Notice& n : playback_->poll_notices()) {
    last_notice_ = n.text;
    if (n.kind == Notice::Kind::SKIPPED) {
      Logger::warn("Music: {}", n.text);
    }
    else {
      Logger::info("Music: {}", n.text);
    }
  }
  const std::uint32_t done = library_->progress().scans_finished;
  if (done != scans_seen_) {
    scans_seen_ = done;
    for (const std::string& p : library_->poll_problems()) {
      Logger::info("Music folders: {}", p);
    }
    refresh_queue();
  }
}

} // namespace music
