/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The Music folders screen's model (music::MusicSystem, music::FolderDraft)
// and Restore original audio, on the real settings store, library and music
// owner.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "MusicFixtures.h"
#include "TestSupport.h"
#include "core/content/Sha256.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/MusicSystem.h"

using music::MusicSystem;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct Places {
  test_support::TempDir profile;
  test_support::TempDir game;
  test_support::TempDir user;
  test_support::TempDir other;
  MusicSystem::Paths paths() const {
    return {.profile = profile.path(),
            .game_dir = game.path(),
            .user_music = user.path()};
  }
};

std::string id_of(const fs::path& file) {
  const auto bytes = music_test::read_file(file);
  return music::track_id_for(core::content::Sha256::hex(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size())));
}

// Ticks the system until the library has finished at least `scans` scans
// and none is waiting.
bool scanned(MusicSystem& sys, std::uint32_t scans) {
  return music_test::wait_until([&] {
    sys.tick();
    const auto p = sys.library().progress();
    return !p.scanning && p.scans_finished >= scans;
  });
}

const music::Collection* named(const std::vector<music::Collection>& all,
                               const std::string& name) {
  const auto it =
      std::find_if(all.begin(), all.end(),
                   [&](const music::Collection& c) { return c.name == name; });
  return it == all.end() ? nullptr : &*it;
}
// The pointer points into the vector, so it must outlive the pointer: a
// temporary one would leave it dangling.
const music::Collection* named(std::vector<music::Collection>&& all,
                               const std::string& name) = delete;

bool has(const std::vector<music::Collection>& all, const std::string& name) {
  return named(all, name) != nullptr;
}

} // namespace

TEST_CASE("several folders of any name, labels, and Save, Cancel and Reset",
          "[music][folders]") {
  Places at;
  music_test::tone_wav(at.user.path() / "Carols" / "a.wav", 440, 0.2);
  music_test::tone_wav(at.game.path() / "Xmas" / "b.wav", 550, 0.2);
  const fs::path spanish = at.other.path() / "Villancicos 2026";
  music_test::tone_wav(spanish / "c.wav", 660, 0.2);
  const fs::path kids = at.other.path() / "Kids' Songs";
  music_test::tone_wav(kids / "d.wav", 330, 0.2);

  audio::AudioManager am;
  std::string kept_id;
  {
    MusicSystem sys(am, at.paths());
    music::FolderDraft& draft = sys.folders();
    // Until the player saves a list, the suggestions: their Music folder and
    // the Xmas folder beside the game.
    REQUIRE(draft.folders().size() == 2);
    CHECK_FALSE(draft.dirty());

    std::string error;
    kept_id = draft.add(spanish, &error);
    REQUIRE_FALSE(kept_id.empty());
    CHECK(draft.add(spanish, &error).empty());
    CHECK(error.find("already") != std::string::npos);
    CHECK(draft.add(spanish / "c.wav", &error).empty());
    CHECK(error.find("not a folder") != std::string::npos);
    CHECK(draft.relabel(kept_id, "Navidad"));
    CHECK(draft.dirty());

    // Cancel: back to the saved list.
    draft.cancel();
    CHECK(draft.folders().size() == 2);
    CHECK_FALSE(draft.dirty());

    kept_id = draft.add(spanish);
    draft.relabel(kept_id, "Navidad");
    const std::string kids_id = draft.add(kids);
    draft.set_include_subfolders(kids_id, false);
    REQUIRE(sys.save_folders());
    CHECK_FALSE(draft.dirty());
    CHECK(sys.store().settings().folders_saved);
    CHECK(sys.store().settings().folders.size() == 4);

    // Reset: the suggestions again, saved only when the player saves.
    draft.reset();
    CHECK(draft.folders().size() == 2);
    CHECK(draft.dirty());
    draft.cancel();
    CHECK(draft.folders().size() == 4);

    REQUIRE(scanned(sys, 1));
    const auto rows = sys.folder_rows();
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
      CHECK(row.available);
      CHECK(row.tracks == 1);
    }
    // A folder's own songs make a collection under the player's label.
    CHECK(has(sys.collections(), "Navidad"));
  }
  // After a restart the same folders and labels, and a new suggestion does
  // not change a saved list.
  fs::create_directories(at.user.path() / "More");
  MusicSystem again(am, at.paths());
  const auto& folders = again.folders().folders();
  REQUIRE(folders.size() == 4);
  const auto it = std::find_if(folders.begin(), folders.end(),
                               [&](const auto& f) { return f.id == kept_id; });
  REQUIRE(it != folders.end());
  CHECK(it->label == "Navidad");
}

TEST_CASE("a moved folder is relinked: its collections, labels and playlists "
          "stay",
          "[music][folders]") {
  Places at;
  const fs::path before = at.other.path() / "Music A";
  const fs::path song =
      music_test::tone_wav(before / "Carols" / "x.wav", 440, 0.2);
  const std::string track = id_of(song);

  audio::AudioManager am;
  MusicSystem sys(am, at.paths());
  music::FolderDraft& draft = sys.folders();
  draft.reset();
  for (const auto& f : std::vector(draft.folders())) {
    draft.remove(f.id);
  }
  const std::string root = draft.add(before);
  REQUIRE(sys.save_folders());
  REQUIRE(scanned(sys, 1));
  const auto first_scan = sys.collections();
  const music::Collection* carols = named(first_scan, "Carols");
  REQUIRE(carols);
  const std::string carols_id = carols->id;
  REQUIRE(sys.rename_collection(carols_id, "Our Carols"));
  const std::string list = sys.store().create_playlist("Eve");
  auto p = *sys.store().playlist(list);
  p.tracks = {track};
  REQUIRE(sys.store().save_playlist(p));

  // Renamed outside the game: the row says it is gone, nothing is lost.
  const fs::path after = at.other.path() / "Music B";
  fs::rename(before, after);
  CHECK_FALSE(sys.folder_rows()[0].available);
  CHECK(sys.store().playlist(list)->tracks == std::vector<std::string>{track});

  std::string error;
  REQUIRE(draft.relink(root, after, &error));
  CHECK(draft.relabel(root, "Our music"));
  REQUIRE(sys.save_folders());
  REQUIRE(scanned(sys, 2));
  CHECK(sys.folder_rows()[0].available);
  const auto collections = sys.collections();
  const music::Collection* same = named(collections, "Our Carols");
  REQUIRE(same);
  CHECK(same->id == carols_id);
  const auto entries =
      music::playlist_entries(*sys.store().playlist(list), sys.library());
  REQUIRE(entries.size() == 1);
  CHECK_FALSE(entries[0].missing);
  // The folder itself was never renamed by the game.
  CHECK(fs::exists(after / "Carols" / "x.wav"));

  // Hiding a collection keeps it, out of sight.
  REQUIRE(sys.hide_collection(carols_id, true));
  CHECK_FALSE(has(sys.collections(), "Our Carols"));
  CHECK(has(sys.collections(true), "Our Carols"));
}

TEST_CASE("Locate: a replaced track is relinked in every playlist",
          "[music][folders]") {
  Places at;
  const fs::path folder = at.other.path() / "Songs";
  const fs::path old_file = music_test::tone_wav(folder / "y.wav", 440, 0.2);
  const std::string old_id = id_of(old_file);
  audio::AudioManager am;
  MusicSystem sys(am, at.paths());
  music::FolderDraft& draft = sys.folders();
  for (const auto& f : std::vector(draft.folders())) {
    draft.remove(f.id);
  }
  draft.add(folder);
  REQUIRE(sys.save_folders());
  REQUIRE(scanned(sys, 1));
  const std::string list = sys.store().create_playlist("Mine");
  auto p = *sys.store().playlist(list);
  p.tracks = {old_id};
  REQUIRE(sys.store().save_playlist(p));

  // Another encoding of the song takes its place.
  fs::remove(old_file);
  const fs::path new_file = music_test::tone_wav(folder / "y2.wav", 440, 0.25);
  sys.library().rescan();
  REQUIRE(scanned(sys, 2));
  const std::string new_id = id_of(new_file);
  CHECK(music::playlist_entries(*sys.store().playlist(list), sys.library())[0]
            .missing);
  CHECK_FALSE(sys.relink_track(old_id, "trk:" + std::string(32, '0')));
  REQUIRE(sys.relink_track(old_id, new_id));
  CHECK(sys.store().playlist(list)->tracks == std::vector<std::string>{new_id});
}

TEST_CASE("Restore original audio mid-game: the FM song goes on exactly, and "
          "the effects never change",
          "[music][folders][route]") {
#if !PLEGACY_ENABLE_MUSIC
  SKIP("built without FM music");
#else
  music_test::PausedAudio a;
  if (!a.ok) {
    SKIP("SDL has no dummy audio driver");
  }
  audio::AudioManager& am = a.am;
  Places at;
  const fs::path m_file = at.other.path() / "effect.m";
  music_test::write_file(m_file, music_test::constant_effect_m(132));
  am.load_m_container(m_file.string());
  am.load_music(audio::MusicId::TEA_SHOPPE, music_test::long_note_midi());
  am.play_music(audio::MusicId::TEA_SHOPPE);
  am.play_sound(audio::SoundId::RESTAURANT_NOISE_1, audio::LoopMode::Loop);
  music_test::FmReference ref(music_test::long_note_midi());
  ref.effect(1024); // the VOC level 132 is 1024 as int16

  MusicSystem sys(am, at.paths());
  CHECK(am.music_generation() == 0); // Original, untouched
  for (int i = 0; i < 6; ++i) {
    REQUIRE(music_test::fill(am) == ref.output());
  }
  const double fm_at = am.music_position();

  // Christmas music, nothing queued: the FM song stops where it is and the
  // effect plays on alone.
  sys.set_mode(music::Mode::CHRISTMAS_BACKGROUND);
  music_test::fill(am); // the cut
  for (int i = 0; i < 4; ++i) {
    REQUIRE(music_test::fill(am) == ref.output_without_music());
  }
  CHECK(am.music_position() == fm_at);

  // Restore original audio: the FM song goes on from there, with the effect.
  sys.restore_original();
  CHECK(sys.mode() == music::Mode::ORIGINAL);
  CHECK(sys.store().settings().mode == music::Mode::ORIGINAL);
  music_test::fill(am);
  ref.output_without_music();
  music_test::fill(am); // the FM fades in
  ref.music();
  ref.output_without_music();
  for (int i = 0; i < 6; ++i) {
    REQUIRE(music_test::fill(am) == ref.output());
  }
#endif
}

TEST_CASE("the mini-player's repeat, shuffle and collapsed bar are saved, and "
          "a folder is new until the list is saved",
          "[music][folders]") {
  Places at;
  music_test::tone_wav(at.other.path() / "Hymns" / "a.wav", 440, 0.2);
  audio::AudioManager am;
  {
    MusicSystem sys(am, at.paths());
    CHECK(sys.bar_collapsed());
    sys.set_bar_collapsed(false);
    sys.set_repeat(music::Repeat::ONE);
    sys.set_shuffle(true);
    CHECK(sys.playback().repeat() == music::Repeat::ONE);
    CHECK(sys.playback().shuffle());

    music::FolderDraft& draft = sys.folders();
    const std::string id = draft.add(at.other.path() / "Hymns");
    REQUIRE_FALSE(id.empty());
    CHECK_FALSE(draft.saved(id));
    for (const auto& row : sys.folder_rows()) {
      if (row.folder.id == id) {
        CHECK(row.available);
        CHECK(row.tracks == 0); // not scanned yet
      }
    }
    REQUIRE(sys.save_folders());
    CHECK(draft.saved(id));
  }
  MusicSystem again(am, at.paths());
  CHECK_FALSE(again.bar_collapsed());
  CHECK(again.playback().repeat() == music::Repeat::ONE);
  CHECK(again.playback().shuffle());
  again.set_bar_collapsed(true);
  CHECK(again.store().settings().bar_collapsed);
}
