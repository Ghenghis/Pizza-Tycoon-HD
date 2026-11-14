/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "core/content/Sha256.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/MusicLibrary.h"
#include "engine/music/MusicPlayback.h"
#include "engine/music/MusicSettings.h"

using music::Collection;
using music::MusicLibrary;
using music::MusicRoot;
using music::TrackInfo;
namespace fs = std::filesystem;

namespace {

// Runs scan steps on this thread, as the worker would, until the scan is
// done.
void scan(MusicLibrary& library) {
  for (int i = 0; i < 100000 && library.run_step(); ++i) {
  }
}

const Collection* named(const std::vector<Collection>& all,
                        const std::string& name) {
  const auto it =
      std::find_if(all.begin(), all.end(),
                   [&](const Collection& c) { return c.name == name; });
  return it == all.end() ? nullptr : &*it;
}

std::string id_of(const fs::path& file) {
  const auto bytes = music_test::read_file(file);
  return music::track_id_for(core::content::Sha256::hex(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size())));
}

// A music folder like a player's: collections, a lone album folder, nested
// discs, a song straight in the root, a Unicode name, a duplicate, a broken
// file and things that are not music.
struct MusicTree {
  test_support::TempDir dir;
  fs::path root = dir.path() / "Music";
  fs::path a, a_copy, mp3, alvin, disc1, disc2, loose, cafe, broken;

  MusicTree() {
    a = music_test::tone_wav(root / "Classic Christmas" / "a.wav", 440, 0.2);
    a_copy = root / "Classic Christmas" / "a copy.wav";
    fs::copy_file(a, a_copy);
    mp3 = root / "Classic Christmas" / "b.mp3";
    fs::copy_file(music_test::fixture("tone-440-stereo-44k.mp3"), mp3);
    alvin = music_test::tone_wav(
        root / "Alvin" / "Christmas with the Chipmunks" / "c.wav", 550, 0.2);
    disc1 = music_test::tone_wav(root / "Mixed" / "Disc 1" / "d.wav", 330, 0.2);
    disc2 = music_test::tone_wav(root / "Mixed" / "Disc 2" / "e.wav", 220, 0.2);
    loose = music_test::tone_wav(root / "loose.wav", 660, 0.1);
    cafe = music_test::tone_wav(root / "Unicode" /
                                    music_test::u8(u8"Caf\u00e9 \u266a 01.wav"),
                                880, 0.1);
    broken = root / "Classic Christmas" / "broken.wav";
    music_test::write_file(broken, std::vector<std::uint8_t>(100, 'x'));
    music_test::write_file(root / "notes.txt",
                           std::vector<std::uint8_t>(10, 'n'));
    music_test::write_file(root / "Classic Christmas" / "list.m3u",
                           std::vector<std::uint8_t>(10, 'h'));
    music_test::write_file(root / "Classic Christmas" / "setup.exe",
                           std::vector<std::uint8_t>(10, 'e'));
  }
};

} // namespace

TEST_CASE("folders become collections of tracks with stable ids",
          "[music][library]") {
  MusicTree tree;
  MusicLibrary library(tree.dir.path() / "profile" / "music-library.json");
  library.set_roots({music::chosen_root(tree.root)});
  scan(library);

  const auto progress = library.progress();
  CHECK_FALSE(progress.scanning);
  CHECK(progress.scans_finished == 1);
  const auto collections = library.collections();
  const Collection* classic = named(collections, "Classic Christmas");
  const Collection* alvin = named(collections, "Alvin");
  const Collection* mixed = named(collections, "Mixed");
  const Collection* loose = named(collections, "Music"); // the root's own songs
  const Collection* unicode = named(collections, "Unicode");
  REQUIRE(classic);
  REQUIRE(alvin);
  REQUIRE(mixed);
  REQUIRE(loose);
  REQUIRE(unicode);
  CHECK(collections.size() == 5);

  // a.wav and its copy are the same bytes: one track, two sources.
  CHECK(classic->tracks.size() == 2);
  const auto a = library.find(id_of(tree.a));
  REQUIRE(a);
  CHECK(a->sources.size() == 2);
  CHECK(a->title == "a");
  CHECK(library.file_of(*a) == tree.root / "Classic Christmas" / "a copy.wav");
  const auto mp3 = library.find(id_of(tree.mp3));
  REQUIRE(mp3);
  CHECK(mp3->title == "Caf\xc3\xa9 \xe2\x99\xaa 01"); // from its tags
  CHECK(mp3->format == music::Format::MP3);
  CHECK(mp3->frames_exact);

  // A lone album folder is grouped under its parent, with the way down.
  CHECK(alvin->breadcrumb == "Alvin \xe2\x80\xba Christmas with the Chipmunks");
  CHECK(alvin->tracks == std::vector<std::string>{id_of(tree.alvin)});
  CHECK(mixed->breadcrumb.empty());
  CHECK(mixed->tracks.size() == 2);
  CHECK(unicode->tracks.size() == 1);
  CHECK(library.find(unicode->tracks[0])->title ==
        "Caf\xc3\xa9 \xe2\x99\xaa 01");

  CHECK(library.tracks().size() == 7);
  const auto problems = library.poll_problems();
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].find("broken.wav") != std::string::npos);
}

TEST_CASE("a moved file keeps its id, and unchanged files are not hashed "
          "again",
          "[music][library]") {
  MusicTree tree;
  const fs::path cache = tree.dir.path() / "profile" / "music-library.json";
  const std::string id = id_of(tree.disc1);
  {
    MusicLibrary library(cache);
    library.set_roots({music::chosen_root(tree.root)});
    scan(library);
    CHECK(library.find(id));
  }
  REQUIRE(fs::exists(cache));

  fs::create_directories(tree.root / "Moved");
  fs::rename(tree.disc1, tree.root / "Moved" / "d.wav");
  MusicLibrary library(cache);
  REQUIRE(library.load_cache());
  library.set_roots({music::chosen_root(tree.root)});
  scan(library);
  const auto d = library.find(id);
  REQUIRE(d);
  CHECK(d->sources.size() == 1);
  CHECK(d->sources[0].relative == fs::path("Moved") / "d.wav");
  CHECK(named(library.collections(), "Moved"));
  // A playlist naming it finds it at its new place.
  const music::Playlist list{.id = "pl:0123456789abcdef",
                             .name = "Mine",
                             .tracks = {id},
                             .revision = 1};
  const auto entries = music::playlist_entries(list, library);
  REQUIRE(entries.size() == 1);
  CHECK_FALSE(entries[0].missing);
}

TEST_CASE("a removed file is shown as missing in a playlist, and unknown ids "
          "are kept",
          "[music][library]") {
  MusicTree tree;
  MusicLibrary library(tree.dir.path() / "music-library.json");
  library.set_roots({music::chosen_root(tree.root)});
  scan(library);
  const std::string gone = id_of(tree.loose);
  const std::string kept = id_of(tree.alvin);
  const std::string unknown = "trk:" + std::string(32, '0');
  fs::remove(tree.loose);
  library.rescan();
  scan(library);
  const music::Playlist list{.id = "pl:0123456789abcdef",
                             .name = "Mine",
                             .tracks = {gone, kept, unknown},
                             .revision = 1};
  const auto entries = music::playlist_entries(list, library);
  REQUIRE(entries.size() == 3);
  CHECK(entries[0].missing);
  CHECK_FALSE(entries[1].missing);
  CHECK(entries[2].missing);
  CHECK(entries[2].id == unknown);
}

TEST_CASE("scans stay in their folders and within their bounds",
          "[music][library]") {
  MusicTree tree;
  SECTION("links and junctions are not followed") {
    test_support::TempDir outside;
    music_test::tone_wav(outside.path() / "elsewhere.wav", 440, 0.1);
    std::error_code ec;
    fs::create_directory_symlink(outside.path(), tree.root / "Linked", ec);
    if (ec) {
      SKIP("cannot make a folder link here: " << ec.message());
    }
    MusicLibrary library(tree.dir.path() / "cache.json");
    library.set_roots({music::chosen_root(tree.root)});
    scan(library);
    CHECK(library.tracks().size() == 7);
    CHECK_FALSE(named(library.collections(), "Linked"));
  }
  SECTION("at most max_files files") {
    music::LibraryBounds bounds;
    bounds.max_files = 3;
    MusicLibrary library(tree.dir.path() / "cache.json", bounds);
    library.set_roots({music::chosen_root(tree.root)});
    scan(library);
    CHECK(library.tracks().size() <= 3);
    const auto problems = library.poll_problems();
    CHECK(
        std::any_of(problems.begin(), problems.end(), [](const std::string& p) {
          return p.find("more than 3") != std::string::npos;
        }));
  }
  SECTION("no deeper than max_depth") {
    music::LibraryBounds bounds;
    bounds.max_depth = 1;
    MusicLibrary library(tree.dir.path() / "cache.json", bounds);
    library.set_roots({music::chosen_root(tree.root)});
    scan(library);
    // Alvin's and Mixed's songs sit two folders down.
    CHECK_FALSE(named(library.collections(), "Alvin"));
    CHECK(named(library.collections(), "Classic Christmas"));
  }
}

TEST_CASE("a scan can be cancelled, and the last finished one stays",
          "[music][library]") {
  MusicTree tree;
  music::LibraryBounds bounds;
  bounds.bytes_per_step = 64; // slow hashing, so there is time to cancel
  MusicLibrary library(tree.dir.path() / "cache.json", bounds);
  library.set_roots({music::chosen_root(tree.root)});
  scan(library);
  const std::size_t before = library.tracks().size();
  REQUIRE(before == 7);

  music_test::tone_wav(tree.root / "Classic Christmas" / "new.wav", 440, 0.5);
  library.rescan();
  for (int i = 0; i < 20; ++i) {
    REQUIRE(library.run_step());
  }
  library.cancel_scan();
  CHECK_FALSE(library.run_step());
  CHECK_FALSE(library.progress().scanning);
  CHECK(library.tracks().size() == before);
  const auto problems = library.poll_problems();
  CHECK(std::any_of(problems.begin(), problems.end(), [](const std::string& p) {
    return p.find("cancelled") != std::string::npos;
  }));
}

TEST_CASE("a chosen folder comes before the game's Xmas folder, never merged "
          "without asking",
          "[music][library]") {
  test_support::TempDir game;
  fs::create_directories(game.path() / "Xmas");
  test_support::TempDir mine;
  const auto xmas = music::game_music_root(game.path());
  REQUIRE(xmas);
  CHECK(xmas->key == "game");
  CHECK_FALSE(music::game_music_root(mine.path()));

  const std::vector<music::FolderChoice> none;
  const std::vector<music::FolderChoice> chosen = {
      {.path = music::utf8_path(mine.path()), .enabled = true}};
  // Only the game's folder: it is used.
  auto roots = music::active_roots(none, std::nullopt, xmas);
  REQUIRE(roots.size() == 1);
  CHECK(roots[0].key == "game");
  // A chosen folder takes precedence; the menu offers the game's one.
  roots = music::active_roots(chosen, std::nullopt, xmas);
  REQUIRE(roots.size() == 1);
  CHECK(roots[0].path == music::chosen_root(mine.path()).path);
  CHECK(music::game_folder_offered(chosen, std::nullopt, xmas));
  // The player combined them, or turned the game's one off.
  CHECK(music::active_roots(chosen, true, xmas).size() == 2);
  CHECK_FALSE(music::game_folder_offered(chosen, true, xmas));
  CHECK(music::active_roots(none, false, xmas).empty());
}

TEST_CASE("the music worker runs the scans", "[music][library]") {
  MusicTree tree;
  MusicLibrary library(tree.dir.path() / "cache.json");
  audio::AudioManager am;
  music::MusicPlayback mp(am, &library);
  mp.set_backend(music::Backend::BUILTIN);
  library.set_roots({music::chosen_root(tree.root)});
  REQUIRE(music_test::wait_until(
      [&] { return library.progress().scans_finished == 1; }));
  CHECK(library.tracks().size() == 7);

  // And it plays a playlist from them; the unknown id is skipped.
  const std::string unknown = "trk:" + std::string(32, '0');
  mp.set_repeat(music::Repeat::OFF);
  mp.queue_playlist(music::Playlist{.id = "pl:0123456789abcdef",
                                    .name = "Mine",
                                    .tracks = {unknown, id_of(tree.alvin)},
                                    .revision = 1});
  mp.set_mode(music::Mode::CHRISTMAS_BACKGROUND);
  std::vector<music::Notice> notices;
  REQUIRE(music_test::wait_until([&] {
    for (auto& n : mp.poll_notices()) {
      notices.push_back(n);
    }
    return std::any_of(notices.begin(), notices.end(),
                       [](const music::Notice& n) {
                         return n.kind == music::Notice::Kind::TRACK_STARTED;
                       });
  }));
  CHECK(notices.front().kind == music::Notice::Kind::SKIPPED);
  CHECK(notices.front().track == unknown);
}
