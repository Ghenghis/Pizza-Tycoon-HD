/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/MusicSettings.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace fs = std::filesystem;
using nlohmann::json;

namespace music {

namespace {

constexpr std::array<std::string_view, 3> repeat_names = {"off", "all", "one"};
constexpr std::array<std::string_view, 3> backend_names = {"auto", "builtin",
                                                           "libvlc"};

template <std::size_t N, typename E>
std::string_view enum_name(const std::array<std::string_view, N>& names, E e) {
  return names[static_cast<std::size_t>(e)];
}

template <std::size_t N, typename E>
E enum_from(const std::array<std::string_view, N>& names, const json& j,
            const char* key, E fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) {
    return fallback;
  }
  for (std::size_t i = 0; i < N; ++i) {
    if (names[i] == it->get<std::string>()) {
      return static_cast<E>(i);
    }
  }
  return fallback;
}

bool flag(const json& j, const char* key, bool fallback) {
  const auto it = j.find(key);
  return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string text(const json& j, const char* key, std::size_t max_bytes) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string()
             ? clean_text(it->get<std::string>(), max_bytes)
             : std::string{};
}

Mode mode_from(const json& j, const char* key, Mode fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) {
    return fallback;
  }
  return mode_from_name(it->get<std::string>()).value_or(fallback);
}

// Keys of namespace ns from an array, at most max of them.
std::vector<std::string> keys(const json& j, const char* key,
                              std::string_view ns, std::size_t max) {
  std::vector<std::string> out;
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array()) {
    return out;
  }
  for (const json& v : *it) {
    if (out.size() == max) {
      break;
    }
    if (v.is_string() && is_key_of(v.get<std::string>(), ns)) {
      out.push_back(v.get<std::string>());
    }
  }
  return out;
}

json playlist_json(const Playlist& p) {
  return {{"id", p.id},
          {"name", p.name},
          {"tracks", p.tracks},
          {"revision", p.revision}};
}

json settings_json(const MusicSettings& s) {
  json folders = json::array();
  for (const FolderChoice& f : s.folders) {
    folders.push_back({{"path", f.path}, {"enabled", f.enabled}});
  }
  json collections = json::array();
  for (const CollectionPrefs& c : s.collections) {
    collections.push_back(
        {{"id", c.id}, {"name", c.name}, {"hidden", c.hidden}});
  }
  json playlists = json::array();
  for (const Playlist& p : s.playlists) {
    playlists.push_back(playlist_json(p));
  }
  return {
      {"mode", std::string(mode_name(s.mode))},
      {"folders", std::move(folders)},
      {"use_game_folder",
       s.use_game_folder ? json(*s.use_game_folder) : json(nullptr)},
      {"collections", std::move(collections)},
      {"playlists", std::move(playlists)},
      {"playlist", s.playlist},
      {"queue_collections", s.queue_collections},
      {"resume", {{"track", s.resume_track}, {"seconds", s.resume_seconds}}},
      {"repeat", std::string(enum_name(repeat_names, s.repeat))},
      {"shuffle", s.shuffle},
      {"collapsed", s.collapsed},
      {"north_pole",
       {{"autostart", s.autostart_north_pole},
        {"keep_everywhere", s.keep_everywhere},
        {"background_after_jukebox", s.background_after_jukebox},
        {"mode", std::string(mode_name(s.north_pole_mode))}}},
      {"backend", std::string(enum_name(backend_names, s.backend))},
  };
}

MusicSettings settings_from(const json& j) {
  MusicSettings s;
  if (!j.is_object()) {
    return s;
  }
  s.mode = mode_from(j, "mode", s.mode);
  if (const auto f = j.find("folders"); f != j.end() && f->is_array()) {
    for (const json& v : *f) {
      if (s.folders.size() == SettingsStore::MAX_FOLDERS) {
        break;
      }
      if (v.is_object() && v.contains("path") && v["path"].is_string()) {
        s.folders.push_back(FolderChoice{.path = v["path"].get<std::string>(),
                                         .enabled = flag(v, "enabled", true)});
      }
    }
  }
  if (const auto g = j.find("use_game_folder");
      g != j.end() && g->is_boolean()) {
    s.use_game_folder = g->get<bool>();
  }
  if (const auto c = j.find("collections"); c != j.end() && c->is_array()) {
    for (const json& v : *c) {
      if (v.is_object() && is_key_of(text(v, "id", 128), "col")) {
        s.collections.push_back(
            CollectionPrefs{.id = text(v, "id", 128),
                            .name = text(v, "name", MAX_NAME_BYTES),
                            .hidden = flag(v, "hidden", false)});
      }
    }
  }
  if (const auto p = j.find("playlists"); p != j.end() && p->is_array()) {
    for (const json& v : *p) {
      if (s.playlists.size() == SettingsStore::MAX_PLAYLISTS) {
        break;
      }
      if (!v.is_object()) {
        continue;
      }
      Playlist pl;
      pl.id = text(v, "id", 128);
      pl.name = text(v, "name", MAX_NAME_BYTES);
      pl.tracks = keys(v, "tracks", "trk", SettingsStore::MAX_PLAYLIST_TRACKS);
      const auto r = v.find("revision");
      pl.revision =
          r != v.end() && r->is_number_unsigned() ? r->get<std::uint32_t>() : 1;
      const bool unique = std::none_of(
          s.playlists.begin(), s.playlists.end(),
          [&](const Playlist& other) { return other.id == pl.id; });
      if (is_key_of(pl.id, "pl") && !pl.name.empty() && unique) {
        s.playlists.push_back(std::move(pl));
      }
    }
  }
  s.playlist = text(j, "playlist", 128);
  if (!is_key_of(s.playlist, "pl")) {
    s.playlist.clear();
  }
  s.queue_collections = keys(j, "queue_collections", "col", 1000);
  if (const auto r = j.find("resume"); r != j.end() && r->is_object()) {
    s.resume_track = text(*r, "track", 128);
    if (!is_key_of(s.resume_track, "trk")) {
      s.resume_track.clear();
    }
    const auto sec = r->find("seconds");
    if (sec != r->end() && sec->is_number()) {
      const double v = sec->get<double>();
      s.resume_seconds = std::isfinite(v) && v > 0.0 ? v : 0.0;
    }
  }
  s.repeat = enum_from(repeat_names, j, "repeat", s.repeat);
  s.shuffle = flag(j, "shuffle", s.shuffle);
  s.collapsed = flag(j, "collapsed", s.collapsed);
  if (const auto n = j.find("north_pole"); n != j.end() && n->is_object()) {
    s.autostart_north_pole = flag(*n, "autostart", false);
    s.keep_everywhere = flag(*n, "keep_everywhere", false);
    s.background_after_jukebox = flag(*n, "background_after_jukebox", false);
    s.north_pole_mode = mode_from(*n, "mode", s.north_pole_mode);
    if (s.north_pole_mode != Mode::CHRISTMAS_BACKGROUND &&
        s.north_pole_mode != Mode::JUKEBOX) {
      s.north_pole_mode = Mode::CHRISTMAS_BACKGROUND;
    }
  }
  s.backend = enum_from(backend_names, j, "backend", s.backend);
  return s;
}

} // namespace

std::vector<PlaylistEntry> playlist_entries(const Playlist& playlist,
                                            const MusicLibrary& library) {
  std::vector<PlaylistEntry> out;
  out.reserve(playlist.tracks.size());
  for (const TrackId& id : playlist.tracks) {
    PlaylistEntry e{.id = id, .track = library.find(id), .missing = true};
    e.missing = !e.track || library.file_of(*e.track).empty();
    out.push_back(std::move(e));
  }
  return out;
}

SettingsStore::SettingsStore(fs::path file)
    : doc_(std::move(file), "pthd-music-settings", SCHEMA) {}

bool SettingsStore::load(std::vector<std::string>* problems) {
  std::vector<std::string> found;
  const auto loaded = doc_.load(&found);
  if (problems) {
    *problems = found;
  }
  if (!loaded) {
    settings_ = MusicSettings{};
    source_.clear();
    return found.empty();
  }
  settings_ = settings_from(loaded->data);
  source_ = loaded->source;
  return true;
}

SaveResult SettingsStore::write(const MusicSettings& next) {
  std::error_code ec;
  fs::create_directories(doc_.file().parent_path(), ec);
  if (const auto written = doc_.save(settings_json(next)); !written) {
    return SaveResult{.error = written.error, .revision = 0};
  }
  settings_ = next;
  source_ = "current";
  return {};
}

SaveResult SettingsStore::save_settings(const MusicSettings& settings) {
  MusicSettings next = settings;
  next.playlists = settings_.playlists;
  if (next.folders.size() > MAX_FOLDERS) {
    return SaveResult{.error = "at most " + std::to_string(MAX_FOLDERS) +
                               " music folders can be chosen",
                      .revision = 0};
  }
  return write(next);
}

std::optional<Playlist> SettingsStore::playlist(const PlaylistId& id) const {
  for (const Playlist& p : settings_.playlists) {
    if (p.id == id) {
      return p;
    }
  }
  return std::nullopt;
}

PlaylistId SettingsStore::create_playlist(std::string name,
                                          std::string* error) {
  PlaylistId id;
  do {
    id = random_key("pl");
  } while (playlist(id));
  const SaveResult saved = save_playlist(
      Playlist{.id = id, .name = std::move(name), .tracks = {}, .revision = 0});
  if (!saved) {
    if (error) {
      *error = saved.error;
    }
    return {};
  }
  return id;
}

SaveResult SettingsStore::save_playlist(const Playlist& playlist) {
  const auto refuse = [](std::string why) {
    return SaveResult{.error = std::move(why), .revision = 0};
  };
  if (!is_key_of(playlist.id, "pl")) {
    return refuse("not a playlist id");
  }
  Playlist p = playlist;
  p.name = clean_text(p.name, MAX_NAME_BYTES);
  if (p.name.empty()) {
    return refuse("the playlist needs a name");
  }
  if (p.tracks.size() > MAX_PLAYLIST_TRACKS) {
    return refuse("a playlist holds at most " +
                  std::to_string(MAX_PLAYLIST_TRACKS) + " tracks");
  }
  for (const TrackId& t : p.tracks) {
    if (!is_key_of(t, "trk")) {
      return refuse("not a track id: " + clean_text(t, 80));
    }
  }
  MusicSettings next = settings_;
  const auto it = std::find_if(next.playlists.begin(), next.playlists.end(),
                               [&](const Playlist& q) { return q.id == p.id; });
  if (it != next.playlists.end()) {
    if (p.revision != it->revision) {
      return refuse("the playlist changed since it was read (revision " +
                    std::to_string(p.revision) + ", now " +
                    std::to_string(it->revision) + ")");
    }
    p.revision = it->revision + 1;
    *it = p;
  }
  else {
    if (p.revision != 0) {
      return refuse("the playlist was removed since it was read");
    }
    if (next.playlists.size() >= MAX_PLAYLISTS) {
      return refuse("there are already " + std::to_string(MAX_PLAYLISTS) +
                    " playlists");
    }
    p.revision = 1;
    next.playlists.push_back(p);
  }
  SaveResult saved = write(next);
  if (saved) {
    saved.revision = p.revision;
  }
  return saved;
}

bool SettingsStore::remove_playlist(const PlaylistId& id) {
  MusicSettings next = settings_;
  const auto it = std::remove_if(next.playlists.begin(), next.playlists.end(),
                                 [&](const Playlist& p) { return p.id == id; });
  if (it == next.playlists.end()) {
    return false;
  }
  next.playlists.erase(it, next.playlists.end());
  if (next.playlist == id) {
    next.playlist.clear();
  }
  return static_cast<bool>(write(next));
}

} // namespace music
