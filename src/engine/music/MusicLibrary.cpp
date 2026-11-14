/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/MusicLibrary.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>

#include "core/content/Sha256.h"
#include "core/util/AtomicFile.h"
#include "engine/Logger.h"
#include "engine/music/JsonStore.h"
#include "engine/music/TrackDecoder.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace music {

namespace {

constexpr std::string_view cache_format = "pthd-music-library-cache";
constexpr int cache_version = 1;
constexpr std::uint64_t max_cache_bytes = std::uint64_t{64} << 20;
// Cached files looked up per step.
constexpr int cache_hits_per_step = 64;
// "Artist \u203A Album", as UTF-8
constexpr std::string_view crumb_separator = " \xE2\x80\xBA ";

std::string hex16(std::string_view text) {
  return core::content::Sha256::hex(text).substr(0, 16);
}

std::string hex_digest(const std::array<std::uint8_t, 32>& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t b : digest) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
  }
  return out;
}

bool is_hex(std::string_view text, std::size_t length) {
  return text.size() == length &&
         std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string generic(const fs::path& relative) {
  const std::u8string text = relative.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string path_key(const std::string& root, const fs::path& relative) {
  return root + "/" + generic(relative);
}

bool audio_extension(const fs::path& file) {
  std::string ext = utf8_path(file.extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return ext == ".mp3" || ext == ".wav";
}

std::int64_t file_time(const fs::directory_entry& entry, std::error_code& ec) {
  return static_cast<std::int64_t>(
      entry.last_write_time(ec).time_since_epoch().count());
}

json track_json(const TrackInfo& t) {
  return {{"id", t.id},
          {"sha256", t.sha256},
          {"bytes", t.bytes},
          {"format", std::string(format_name(t.format))},
          {"rate", t.sample_rate},
          {"channels", t.channels},
          {"frames", t.frames},
          {"exact", t.frames_exact},
          {"kbps", t.bitrate_kbps},
          {"encoding", t.encoding},
          {"title", t.title},
          {"artist", t.artist},
          {"album", t.album}};
}

template <typename T> T number(const json& j, const char* key, T fallback) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number_unsigned() ? it->get<T>() : fallback;
}

std::string text(const json& j, const char* key, std::size_t max_bytes) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string()
             ? clean_text(it->get<std::string>(), max_bytes)
             : std::string{};
}

std::optional<TrackInfo> track_from_json(const json& j) {
  TrackInfo t;
  t.sha256 = text(j, "sha256", 64);
  t.id = text(j, "id", 128);
  const auto format = format_from_name(text(j, "format", 8));
  if (!is_hex(t.sha256, 64) || t.id != track_id_for(t.sha256) || !format) {
    return std::nullopt;
  }
  t.format = *format;
  t.bytes = number<std::uint64_t>(j, "bytes", 0);
  t.sample_rate = number<std::uint32_t>(j, "rate", 0);
  t.channels = number<std::uint32_t>(j, "channels", 0);
  t.frames = number<std::uint64_t>(j, "frames", 0);
  t.frames_exact = j.value("exact", false);
  t.bitrate_kbps = number<std::uint32_t>(j, "kbps", 0);
  t.encoding = text(j, "encoding", 64);
  t.title = text(j, "title", MAX_TITLE_BYTES);
  t.artist = text(j, "artist", MAX_TITLE_BYTES);
  t.album = text(j, "album", MAX_TITLE_BYTES);
  if (t.bytes == 0 || t.sample_rate == 0 || t.channels == 0) {
    return std::nullopt;
  }
  return t;
}

} // namespace

/* Roots */

std::optional<MusicRoot> game_music_root(const fs::path& game_dir) {
  std::error_code ec;
  const fs::path folder = game_dir / "Xmas";
  if (game_dir.empty() || !fs::is_directory(folder, ec)) {
    return std::nullopt;
  }
  return MusicRoot{.key = "game", .path = folder, .name = "Xmas"};
}

MusicRoot chosen_root(const fs::path& folder) {
  std::error_code ec;
  fs::path path = fs::absolute(folder, ec);
  if (ec) {
    path = folder;
  }
  path = path.lexically_normal();
  if (!path.has_filename() && path.has_parent_path() &&
      path.parent_path() != path) {
    path = path.parent_path();
  }
  return MusicRoot{.key = "folder:" + hex16(utf8_path(path)),
                   .path = path,
                   .name =
                       clean_text(utf8_path(path.filename()), MAX_NAME_BYTES)};
}

std::vector<MusicRoot> active_roots(const std::vector<FolderChoice>& chosen,
                                    std::optional<bool> use_game_folder,
                                    const std::optional<MusicRoot>& game) {
  std::vector<MusicRoot> roots;
  std::set<std::string> keys;
  for (const FolderChoice& c : chosen) {
    if (c.enabled && !c.path.empty()) {
      MusicRoot root = chosen_root(path_from_utf8(c.path));
      if (keys.insert(root.key).second) {
        roots.push_back(std::move(root));
      }
    }
  }
  if (game && use_game_folder.value_or(roots.empty())) {
    roots.push_back(*game);
  }
  return roots;
}

bool game_folder_offered(const std::vector<FolderChoice>& chosen,
                         std::optional<bool> use_game_folder,
                         const std::optional<MusicRoot>& game) {
  return game && !use_game_folder &&
         std::any_of(chosen.begin(), chosen.end(), [](const FolderChoice& c) {
           return c.enabled && !c.path.empty();
         });
}

/* The scan */

struct MusicLibrary::Scan {
  struct Dir {
    std::size_t root = 0;
    fs::path relative;
    int depth = 0;
  };
  struct Found {
    std::size_t root = 0;
    fs::path relative;
    std::uint64_t bytes = 0;
    std::int64_t time = 0;
  };

  std::vector<MusicRoot> roots;
  std::map<std::string, Cached> old_cache;
  std::vector<Dir> dirs;
  std::optional<fs::directory_iterator> it;
  Dir at;
  std::vector<Found> found;
  bool listed = false;
  std::size_t next = 0;

  // The file being hashed.
  std::ifstream in;
  std::optional<core::content::Sha256> hash;
  std::uint64_t hashed = 0;
  StreamInfo info;
  std::vector<char> buffer;

  std::map<TrackId, TrackInfo> tracks;
  std::map<std::string, Cached> cache;
  std::size_t refused = 0;
};

MusicLibrary::MusicLibrary(fs::path index_file, Bounds bounds)
    : index_file_(std::move(index_file)), bounds_(bounds) {}

MusicLibrary::~MusicLibrary() = default;

bool MusicLibrary::load_cache(std::string* error) {
  std::string why;
  const std::optional<std::string> body =
      read_text_file(index_file_, max_cache_bytes, &why);
  if (!body) {
    if (error) {
      *error = why;
    }
    return why.empty();
  }
  const std::optional<json> doc = parse_json(*body, 8, 2'000'000, &why);
  if (!doc || !doc->is_object() || doc->value("format", "") != cache_format ||
      doc->value("version", 0) != cache_version || !doc->contains("files") ||
      !(*doc)["files"].is_array()) {
    if (error) {
      *error = "the music cache is damaged" + (why.empty() ? "" : ": " + why);
    }
    return false;
  }
  std::map<std::string, Cached> cache;
  for (const json& f : (*doc)["files"]) {
    if (!f.is_object()) {
      continue;
    }
    const std::string key = text(f, "path", 4096);
    const auto track = track_from_json(f);
    if (key.empty() || !track) {
      continue;
    }
    const auto time_it = f.find("time");
    cache[key] = Cached{
        .bytes = track->bytes,
        .time = time_it != f.end() && time_it->is_number_integer()
                    ? time_it->get<std::int64_t>()
                    : 0,
        .track = *track,
    };
  }
  const std::lock_guard lock(mutex_);
  cache_ = std::move(cache);
  return true;
}

void MusicLibrary::set_roots(std::vector<MusicRoot> roots) {
  {
    const std::lock_guard lock(mutex_);
    roots_ = std::move(roots);
  }
  request_scan();
}

void MusicLibrary::rescan() { request_scan(); }

void MusicLibrary::request_scan() {
  std::function<void()> wake;
  {
    const std::lock_guard lock(mutex_);
    scan_requested_ = true;
    progress_.scanning = true;
    wake = wake_;
  }
  if (wake) {
    wake();
  }
}

void MusicLibrary::cancel_scan() {
  cancel_.store(true);
  std::function<void()> wake;
  {
    const std::lock_guard lock(mutex_);
    scan_requested_ = false;
    wake = wake_;
  }
  if (wake) {
    wake();
  }
}

MusicLibrary::Progress MusicLibrary::progress() const {
  const std::lock_guard lock(mutex_);
  return progress_;
}

std::vector<MusicRoot> MusicLibrary::roots() const {
  const std::lock_guard lock(mutex_);
  return roots_;
}

std::vector<Collection> MusicLibrary::collections() const {
  const std::lock_guard lock(mutex_);
  return collections_;
}

std::vector<TrackInfo> MusicLibrary::tracks() const {
  const std::lock_guard lock(mutex_);
  std::vector<TrackInfo> out;
  out.reserve(tracks_.size());
  for (const auto& [id, t] : tracks_) {
    out.push_back(t);
  }
  return out;
}

std::optional<TrackInfo> MusicLibrary::find(const TrackId& id) const {
  const std::lock_guard lock(mutex_);
  const auto it = tracks_.find(id);
  if (it == tracks_.end()) {
    return std::nullopt;
  }
  return it->second;
}

fs::path MusicLibrary::file_of(const TrackInfo& track) const {
  const std::lock_guard lock(mutex_);
  for (const TrackSource& s : track.sources) {
    for (const MusicRoot& r : roots_) {
      if (r.key == s.root) {
        return r.path / s.relative;
      }
    }
  }
  return {};
}

std::vector<std::string> MusicLibrary::poll_problems() {
  const std::lock_guard lock(mutex_);
  std::vector<std::string> out = std::move(problems_);
  problems_.clear();
  if (problems_dropped_ > 0) {
    out.push_back("and " + std::to_string(problems_dropped_) + " more");
    problems_dropped_ = 0;
  }
  return out;
}

void MusicLibrary::attach_worker(std::function<void()> wake) {
  std::function<void()> due;
  {
    const std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
    if (scan_requested_) {
      due = wake_;
    }
  }
  if (due) {
    due();
  }
}

void MusicLibrary::problem(std::string text) {
  const std::lock_guard lock(mutex_);
  if (problems_.size() < bounds_.max_problems) {
    problems_.push_back(std::move(text));
  }
  else {
    ++problems_dropped_;
  }
}

bool MusicLibrary::run_step() {
  if (cancel_.exchange(false) && scan_) {
    scan_.reset();
    problem("The music folder scan was cancelled");
    const std::lock_guard lock(mutex_);
    progress_.scanning = scan_requested_;
  }
  {
    const std::lock_guard lock(mutex_);
    if (scan_requested_) {
      scan_requested_ = false;
      scan_ = std::make_unique<Scan>();
      scan_->roots = roots_;
      scan_->old_cache = cache_;
      for (std::size_t i = 0; i < scan_->roots.size(); ++i) {
        scan_->dirs.push_back(Scan::Dir{.root = i, .relative = {}, .depth = 0});
      }
      scan_->buffer.resize(bounds_.bytes_per_step);
      progress_ = Progress{.scanning = true,
                           .scans_finished = progress_.scans_finished};
    }
  }
  if (!scan_) {
    return false;
  }
  Scan& s = *scan_;

  if (!s.listed) {
    // List a bounded number of folder entries.
    for (std::size_t entries = 0; entries < bounds_.entries_per_step;) {
      if (!s.it) {
        if (s.dirs.empty()) {
          s.listed = true;
          break;
        }
        s.at = s.dirs.back();
        s.dirs.pop_back();
        std::error_code ec;
        const fs::path folder = s.roots[s.at.root].path / s.at.relative;
        s.it.emplace(folder, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
          problem("The folder " + utf8_path(folder) + " cannot be read");
          s.it.reset();
        }
        continue;
      }
      if (*s.it == fs::directory_iterator()) {
        s.it.reset();
        continue;
      }
      const fs::directory_entry entry = **s.it;
      std::error_code ec;
      s.it->increment(ec);
      if (ec) {
        s.it.reset();
      }
      ++entries;
      // The entry itself, links not followed: a link or a junction is
      // neither a folder nor a file here, so a scan never leaves the roots.
      const fs::file_status status = entry.symlink_status(ec);
      if (ec) {
        continue;
      }
      const fs::path relative = s.at.relative / entry.path().filename();
      if (fs::is_directory(status)) {
        if (s.at.depth < bounds_.max_depth) {
          s.dirs.push_back(Scan::Dir{.root = s.at.root,
                                     .relative = relative,
                                     .depth = s.at.depth + 1});
        }
        continue;
      }
      if (!fs::is_regular_file(status) || !audio_extension(relative)) {
        continue;
      }
      if (s.found.size() >= bounds_.max_files) {
        problem("There are more than " + std::to_string(bounds_.max_files) +
                " music files; the rest are left out");
        s.dirs.clear();
        s.it.reset();
        s.listed = true;
        break;
      }
      const std::uint64_t bytes = entry.file_size(ec);
      const std::int64_t time = ec ? 0 : file_time(entry, ec);
      s.found.push_back(Scan::Found{.root = s.at.root,
                                    .relative = relative,
                                    .bytes = bytes,
                                    .time = time});
    }
    if (s.listed) {
      std::sort(s.found.begin(), s.found.end(),
                [](const Scan::Found& a, const Scan::Found& b) {
                  return a.root != b.root
                             ? a.root < b.root
                             : generic(a.relative) < generic(b.relative);
                });
    }
  }
  else if (s.next < s.found.size()) {
    const auto add = [&s](const Scan::Found& f, TrackInfo track) {
      const std::string& root = s.roots[f.root].key;
      track.sources.clear();
      s.cache[path_key(root, f.relative)] =
          Cached{.bytes = f.bytes, .time = f.time, .track = track};
      auto [it, added] = s.tracks.try_emplace(track.id, std::move(track));
      it->second.sources.push_back(
          TrackSource{.root = root, .relative = f.relative});
    };

    if (!s.hash) {
      // Files hashed before, unchanged, cost a lookup; the next new one is
      // checked and its hashing begins.
      for (int n = 0; n < cache_hits_per_step && s.next < s.found.size(); ++n) {
        const Scan::Found& f = s.found[s.next];
        const auto cached =
            s.old_cache.find(path_key(s.roots[f.root].key, f.relative));
        if (cached != s.old_cache.end() && cached->second.bytes == f.bytes &&
            cached->second.time == f.time) {
          add(f, cached->second.track);
          ++s.next;
          continue;
        }
        const fs::path file = s.roots[f.root].path / f.relative;
        std::string why;
        const std::optional<StreamInfo> info =
            probe_file(file, bounds_.limits, &why);
        if (!info) {
          problem(generic(f.relative) + ": " + why);
          ++s.refused;
          ++s.next;
          continue;
        }
        s.in.open(file, std::ios::binary);
        if (!s.in) {
          problem(generic(f.relative) + ": the file cannot be read");
          s.in.clear();
          ++s.refused;
          ++s.next;
          continue;
        }
        s.info = *info;
        s.hash.emplace();
        s.hashed = 0;
        break;
      }
    }
    else {
      const Scan::Found& f = s.found[s.next];
      s.in.read(s.buffer.data(), static_cast<std::streamsize>(s.buffer.size()));
      const auto n = static_cast<std::size_t>(s.in.gcount());
      s.hash->update(std::string_view(s.buffer.data(), n));
      s.hashed += n;
      if (n == 0 || s.in.eof() || !s.in) {
        const bool read_all = s.in.eof() && s.hashed == f.bytes;
        s.in.close();
        s.in.clear();
        const std::string sha = hex_digest(s.hash->finish());
        s.hash.reset();
        ++s.next;
        if (!read_all) {
          problem(generic(f.relative) + ": the file changed while it was read");
          ++s.refused;
        }
        else {
          TrackInfo t;
          t.id = track_id_for(sha);
          t.sha256 = sha;
          t.bytes = f.bytes;
          t.format = s.info.format;
          t.sample_rate = s.info.sample_rate;
          t.channels = s.info.channels;
          t.frames = s.info.frames;
          t.frames_exact = s.info.frames_exact;
          t.bitrate_kbps = s.info.bitrate_kbps;
          t.encoding = s.info.encoding;
          t.title =
              s.info.title.empty()
                  ? clean_text(utf8_path(f.relative.stem()), MAX_TITLE_BYTES)
                  : s.info.title;
          t.artist = s.info.artist;
          t.album = s.info.album;
          add(f, std::move(t));
        }
      }
    }
  }
  else {
    finish_scan();
    return false;
  }

  const std::lock_guard lock(mutex_);
  progress_.files_found = s.found.size();
  progress_.files_done = s.next;
  progress_.tracks = s.tracks.size();
  progress_.refused = s.refused;
  return true;
}

void MusicLibrary::finish_scan() {
  Scan& s = *scan_;
  struct Building {
    Collection c;
    std::vector<std::pair<std::string, TrackId>> by_path;
    std::optional<fs::path> common; // folder all its songs share
  };
  std::map<std::string, Building> building;
  for (const auto& [id, track] : s.tracks) {
    for (const TrackSource& source : track.sources) {
      const auto root =
          std::find_if(s.roots.begin(), s.roots.end(), [&](const MusicRoot& r) {
            return r.key == source.root;
          });
      if (root == s.roots.end()) {
        continue;
      }
      const fs::path parent = source.relative.parent_path();
      const fs::path top = parent.empty() ? fs::path{} : *parent.begin();
      const std::string key = path_key(source.root, top);
      Building& b = building[key];
      if (b.c.id.empty()) {
        b.c.id = "col:" + hex16(key);
        b.c.root = source.root;
        b.c.folder = top;
        b.c.name = top.empty() ? root->name
                               : clean_text(utf8_path(top), MAX_NAME_BYTES);
        b.common = parent;
      }
      else if (b.common) {
        // Narrow to the folders both paths share.
        fs::path shared;
        auto a = b.common->begin();
        auto c = parent.begin();
        for (; a != b.common->end() && c != parent.end() && *a == *c;
             ++a, ++c) {
          shared /= *a;
        }
        b.common = shared;
      }
      b.by_path.emplace_back(generic(source.relative), id);
      if (track.sample_rate > 0) {
        b.c.frames += track.frames * 44100 / track.sample_rate;
      }
    }
  }
  std::vector<Collection> collections;
  for (auto& [key, b] : building) {
    std::sort(b.by_path.begin(), b.by_path.end());
    for (auto& [path, id] : b.by_path) {
      if (std::find(b.c.tracks.begin(), b.c.tracks.end(), id) ==
          b.c.tracks.end()) {
        b.c.tracks.push_back(id);
      }
    }
    // A lone album folder: name the way down to it.
    if (b.common && std::distance(b.common->begin(), b.common->end()) > 1) {
      std::string crumb;
      for (const fs::path& part : *b.common) {
        if (!crumb.empty()) {
          crumb += crumb_separator;
        }
        crumb += clean_text(utf8_path(part), MAX_NAME_BYTES);
      }
      b.c.breadcrumb = crumb;
    }
    collections.push_back(std::move(b.c));
  }
  std::sort(collections.begin(), collections.end(),
            [](const Collection& a, const Collection& b) {
              return a.name != b.name ? a.name < b.name : a.id < b.id;
            });
  {
    const std::lock_guard lock(mutex_);
    tracks_ = std::move(s.tracks);
    collections_ = std::move(collections);
    cache_ = std::move(s.cache);
    progress_.scanning = scan_requested_;
    progress_.tracks = tracks_.size();
    progress_.collections = collections_.size();
    progress_.files_done = s.found.size();
    progress_.files_found = s.found.size();
    progress_.refused = s.refused;
    ++progress_.scans_finished;
  }
  scan_.reset();
  write_cache();
}

void MusicLibrary::write_cache() {
  json files = json::array();
  {
    const std::lock_guard lock(mutex_);
    for (const auto& [key, cached] : cache_) {
      json f = track_json(cached.track);
      f["path"] = key;
      f["time"] = cached.time;
      files.push_back(std::move(f));
    }
  }
  const json doc = {{"format", cache_format},
                    {"version", cache_version},
                    {"files", std::move(files)}};
  std::error_code ec;
  fs::create_directories(index_file_.parent_path(), ec);
  if (const auto written = util::write_file_atomically(index_file_, doc.dump());
      !written) {
    Logger::warn("Music cache not saved: {}", written.error);
  }
}

} // namespace music
