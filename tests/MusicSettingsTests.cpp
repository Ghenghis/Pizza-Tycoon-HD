/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "engine/music/MusicSettings.h"

using music::MusicSettings;
using music::Playlist;
using music::SettingsStore;
namespace fs = std::filesystem;

namespace {

void overwrite(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string slurp(const fs::path& file) {
  const auto bytes = music_test::read_file(file);
  return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("a first start has the defaults", "[music][settings]") {
  test_support::TempDir dir;
  SettingsStore store(dir.path() / "music" / "music-settings.json");
  std::vector<std::string> problems;
  CHECK(store.load(&problems));
  CHECK(problems.empty());
  CHECK(store.loaded_from().empty());
  CHECK(store.settings().mode == music::Mode::ORIGINAL);
  CHECK(store.settings().bar_collapsed);
  CHECK(store.settings().backend == music::Backend::BUILTIN);
  CHECK_FALSE(store.settings().folders_saved);
  CHECK(store.settings().playlists.empty());
}

TEST_CASE("settings and playlists survive a restart", "[music][settings]") {
  test_support::TempDir dir;
  const fs::path file = dir.path() / "music" / "music-settings.json";
  const std::string unknown = "trk:" + std::string(32, '7');
  std::string id;
  {
    SettingsStore store(file);
    REQUIRE(store.load());
    MusicSettings s = store.settings();
    s.mode = music::Mode::JUKEBOX;
    s.folders = {{.id = "root:0123456789abcdef",
                  .path = music::utf8_path(
                      dir.path() / music_test::u8(u8"Caf\u00e9 \u266a")),
                  .label = "Weihnachten",
                  .enabled = true,
                  .include_subfolders = false},
                 {.id = "root:game-xmas",
                  .path = "@game/Xmas",
                  .label = {},
                  .enabled = false,
                  .include_subfolders = true}};
    s.folders_saved = true;
    s.collections = {
        {.id = "col:0123456789abcdef", .name = "Kids", .hidden = true}};
    s.last_track = unknown;
    s.resume_seconds = 12.5;
    s.repeat = music::Repeat::ONE;
    s.shuffle = true;
    s.bar_collapsed = false;
    s.autostart_north_pole = true;
    s.keep_everywhere = true;
    s.north_pole_mode = music::Mode::JUKEBOX;
    s.backend = music::Backend::LIBVLC;
    REQUIRE(store.save_settings(s));
    id = store.create_playlist("Christmas Eve");
    REQUIRE(!id.empty());
    CHECK(music::is_key_of(id, "pl"));
    auto p = *store.playlist(id);
    CHECK(p.revision == 1);
    p.tracks = {unknown, "trk:" + std::string(32, 'a')};
    REQUIRE(store.save_playlist(p).revision == 2);
  }
  SettingsStore again(file);
  REQUIRE(again.load());
  CHECK(again.loaded_from() == "current");
  const MusicSettings& s = again.settings();
  CHECK(s.mode == music::Mode::JUKEBOX);
  REQUIRE(s.folders.size() == 2);
  CHECK(s.folders[0].path ==
        music::utf8_path(dir.path() / music_test::u8(u8"Caf\u00e9 \u266a")));
  CHECK(s.folders[0].label == "Weihnachten");
  CHECK_FALSE(s.folders[0].include_subfolders);
  CHECK(s.folders[1].path == "@game/Xmas");
  CHECK_FALSE(s.folders[1].enabled);
  CHECK(s.folders_saved);
  REQUIRE(s.collections.size() == 1);
  CHECK(s.collections[0].hidden);
  CHECK(s.last_track == unknown);
  CHECK(s.resume_seconds == 12.5);
  CHECK(s.repeat == music::Repeat::ONE);
  CHECK(s.shuffle);
  CHECK_FALSE(s.bar_collapsed);
  CHECK(s.autostart_north_pole);
  CHECK(s.keep_everywhere);
  CHECK(s.north_pole_mode == music::Mode::JUKEBOX);
  CHECK(s.backend == music::Backend::LIBVLC);
  const auto p = again.playlist(id);
  REQUIRE(p);
  CHECK(p->name == "Christmas Eve");
  CHECK(p->revision == 2);
  // An id the library may not know is kept as it was.
  CHECK(p->tracks ==
        std::vector<std::string>{unknown, "trk:" + std::string(32, 'a')});
}

TEST_CASE("a stale or invalid playlist save is refused", "[music][settings]") {
  test_support::TempDir dir;
  SettingsStore store(dir.path() / "music-settings.json");
  REQUIRE(store.load());
  const std::string id = store.create_playlist("Mine");
  REQUIRE(!id.empty());
  Playlist read_first = *store.playlist(id);
  Playlist read_second = read_first;

  read_first.tracks = {"trk:" + std::string(32, 'b')};
  REQUIRE(store.save_playlist(read_first));
  // The second editor read it before that save: refused, not merged.
  read_second.name = "Theirs";
  const auto stale = store.save_playlist(read_second);
  CHECK_FALSE(stale);
  CHECK(stale.error.find("changed since it was read") != std::string::npos);
  CHECK(store.playlist(id)->name == "Mine");

  Playlist p = *store.playlist(id);
  p.name = "   ";
  CHECK(store.save_playlist(p).error.find("needs a name") != std::string::npos);
  p.name = "Ok";
  p.tracks = {"not an id"};
  CHECK(store.save_playlist(p).error.find("not a track id") !=
        std::string::npos);
  p.tracks.clear();
  p.name = std::string(98, 'x') + "\xc3\xa9\xc3\xa9"; // 102 bytes
  REQUIRE(store.save_playlist(p));
  CHECK(store.playlist(id)->name == std::string(98, 'x') + "\xc3\xa9");

  Playlist other{
      .id = "pl:00000000000000ff", .name = "Gone", .tracks = {}, .revision = 4};
  CHECK(store.save_playlist(other).error.find("removed") != std::string::npos);
  CHECK(store.remove_playlist(id));
  CHECK_FALSE(store.playlist(id));
  CHECK_FALSE(store.remove_playlist(id));
}

TEST_CASE("three working copies stay behind, and a damaged file falls back "
          "to the newest good one",
          "[music][settings]") {
  test_support::TempDir dir;
  const fs::path file = dir.path() / "music-settings.json";
  {
    SettingsStore store(file);
    REQUIRE(store.load());
    MusicSettings s = store.settings();
    for (int i = 0; i < 5; ++i) {
      s.resume_seconds = i;
      REQUIRE(store.save_settings(s));
    }
  }
  const SettingsStore probe(file);
  CHECK(fs::exists(file));
  CHECK(fs::exists(probe.document().backup_path(1)));
  CHECK(fs::exists(probe.document().backup_path(2)));
  CHECK(fs::exists(probe.document().backup_path(3)));
  CHECK_FALSE(fs::exists(probe.document().backup_path(4)));
  CHECK_FALSE(fs::exists(probe.document().candidate_path()));

  SECTION("cut short") {
    const std::string text = slurp(file);
    overwrite(file, text.substr(0, text.size() / 2));
  }
  SECTION("edited without its checksum") {
    std::string text = slurp(file);
    const auto at = text.find("\"seconds\": 4");
    REQUIRE(at != std::string::npos);
    text.replace(at, 12, "\"seconds\": 9");
    overwrite(file, text);
  }
  SettingsStore store(file);
  std::vector<std::string> problems;
  REQUIRE(store.load(&problems));
  CHECK(store.loaded_from() == "backup 1");
  CHECK(store.settings().resume_seconds == 3.0);
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].find("current") != std::string::npos);

  // The next save sets the damaged copy aside, not into the backups.
  REQUIRE(store.save_settings(store.settings()));
  CHECK(fs::exists(store.document().damaged_path()));
  SettingsStore after(file);
  REQUIRE(after.load());
  CHECK(after.loaded_from() == "current");
}

TEST_CASE("a save cut off before it replaced the file is recovered",
          "[music][settings]") {
  test_support::TempDir dir;
  const fs::path file = dir.path() / "music-settings.json";
  SettingsStore store(file);
  REQUIRE(store.load());
  MusicSettings s = store.settings();
  s.resume_seconds = 1;
  REQUIRE(store.save_settings(s));
  s.resume_seconds = 2;
  REQUIRE(store.save_settings(s));

  // The checked candidate was written, then the power went before the
  // current file was replaced: the current file is still good and wins.
  fs::copy_file(file, store.document().candidate_path());
  overwrite(store.document().candidate_path(), "{\"format\":");
  SettingsStore a(file);
  REQUIRE(a.load());
  CHECK(a.loaded_from() == "current");
  CHECK(a.settings().resume_seconds == 2.0);

  // Lost between moving the current file away and moving the candidate in:
  // the candidate is what was being saved.
  fs::copy_file(file, store.document().candidate_path(),
                fs::copy_options::overwrite_existing);
  fs::remove(file);
  SettingsStore b(file);
  REQUIRE(b.load());
  CHECK(b.loaded_from() == "candidate");
  CHECK(b.settings().resume_seconds == 2.0);

  // Nothing works: the defaults, and the reasons.
  for (const fs::path& p :
       {store.document().candidate_path(), store.document().backup_path(1)}) {
    overwrite(p, "not json");
  }
  SettingsStore c(file);
  std::vector<std::string> problems;
  CHECK_FALSE(c.load(&problems));
  CHECK(problems.size() == 2);
  CHECK(c.settings().resume_seconds == 0.0);
}
