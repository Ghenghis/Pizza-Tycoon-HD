/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The native jukebox (furniture JUKEBOX 138, jukebox_music 0..6) is
// gameplay: saved, and +12 / -4 appeal to the matching customers. The
// player's own music (R35: folders, collections, playlists, modes) is
// cosmetic and kept apart: nothing it does may change appeal or a save.
// The jukebox's song is heard only while a jukebox stands.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>

#include "ContentJourney.h"
#include "ContentTestGame.h"
#include "MusicFixtures.h"
#include "core/factories/FurnitureFactory.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"
#include "core/save/SaveGameManager.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/MusicSystem.h"
#include "views/screens/restaurant/JukeboxAudio.h"

namespace fs = std::filesystem;

namespace {

std::string fingerprint(content_test::TestGame& game) {
  return save::SaveGameManager::fingerprint(game.state, game.context).hash;
}

// What Classic gives: furniture appeal, +12 for the jukebox's demographic
// and -4 for the others (and the same for the TV), halved and clamped.
FurnitureAppeal classic_appeal(const Restaurant& r) {
  const auto base = r.economy().calculate_furniture_popularity(r);
  FurnitureAppeal out;
  const int juke = r.jukebox_music();
  const int tv = r.economy().tv_channel();
  for (const ConsumerType d : ALL_CONSUMER_TYPES) {
    const int i = static_cast<int>(d);
    int v = base[d];
    if (juke > 0) {
      v += juke - 1 == i ? 12 : -4;
    }
    if (tv > 0) {
      v += tv - 1 == i ? 12 : -4;
    }
    out[d] = std::clamp(v / 2, 0, 150);
  }
  return out;
}

void place_jukebox(Restaurant& r) {
  const Furniture* juke =
      FurnitureFactory::create_furniture(FurnitureType::JUKEBOX);
  REQUIRE(juke);
  REQUIRE(juke->role() == FurnitureRole::Jukebox);
  auto& layout = *r.layout();
  layout.place_furniture(std::make_unique<PlacedFurniture>(
      juke, layout.next_furniture_id(), WorldPoint{40, 40},
      core::FurnitureId{UNLINKED_FURNITURE}, 0));
}

} // namespace

TEST_CASE("the player's own music never changes appeal or the save; the "
          "native jukebox changes appeal as in Classic",
          "[music][jukebox][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const auto j =
      content_journey::set_up_shop(game, content_journey::Fit::PieceByPiece);
  Restaurant& r = *j.trading;
  place_jukebox(r);
  r.jukebox_music(3);
  const FurnitureAppeal appeal = r.calculate_furniture_appeal_popularity();
  const std::string saved = fingerprint(game);

  // Every kind of R35 action, with the music system in the game's context.
  test_support::TempDir profile;
  test_support::TempDir game_dir;
  test_support::TempDir user;
  music_test::tone_wav(user.path() / "Carols" / "a.wav", 440, 0.3);
  music_test::tone_wav(user.path() / "Carols" / "b.wav", 550, 0.3);
  audio::AudioManager am;
  {
    music::MusicSystem sys(am, {.profile = profile.path(),
                                .game_dir = game_dir.path(),
                                .user_music = user.path()});
    game.context.music = &sys;
    for (const music::Mode m :
         {music::Mode::CHRISTMAS_BACKGROUND, music::Mode::JUKEBOX,
          music::Mode::OFF, music::Mode::ORIGINAL,
          music::Mode::CHRISTMAS_BACKGROUND}) {
      sys.set_mode(m);
    }
    REQUIRE(music_test::wait_until([&] {
      sys.tick();
      const auto p = sys.library().progress();
      return !p.scanning && p.scans_finished >= 1;
    }));
    sys.playback().play();
    sys.playback().next();
    sys.playback().previous();
    sys.playback().seek(0.1);
    sys.playback().pause();
    sys.set_repeat(music::Repeat::ONE);
    sys.set_shuffle(true);
    sys.set_bar_collapsed(false);
    const auto collections = sys.collections();
    REQUIRE_FALSE(collections.empty());
    REQUIRE(sys.rename_collection(collections.front().id, "Our Carols"));
    const std::string list = sys.store().create_playlist("Eve");
    auto p = *sys.store().playlist(list);
    p.tracks = collections.front().tracks;
    REQUIRE(sys.store().save_playlist(p));
    sys.playback().queue_playlist(p);
    music::FolderDraft& draft = sys.folders();
    REQUIRE_FALSE(draft.add(game_dir.path()).empty());
    REQUIRE(sys.save_folders());
    // The restaurant hooks only read the native choice.
    sys.playback().on_restaurant_enter(r.jukebox_music());
    sys.playback().on_restaurant_jukebox(r.jukebox_music());
    sys.playback().on_restaurant_leave();
    sys.restore_original();

    CHECK(r.jukebox_music() == 3);
    CHECK(r.calculate_furniture_appeal_popularity().data() == appeal.data());
    CHECK(fingerprint(game) == saved);
    game.context.music = nullptr;
  }
  CHECK(fingerprint(game) == saved);

  // The native choice: each station favours its own demographic, exactly
  // as Classic does, and the save keeps it.
  for (int choice = 0; choice <= 6; ++choice) {
    r.jukebox_music(choice);
    INFO("jukebox_music " << choice);
    CHECK(r.calculate_furniture_appeal_popularity().data() ==
          classic_appeal(r).data());
    if (choice != 3) {
      CHECK(fingerprint(game) != saved);
    }
  }
  r.jukebox_music(3);
  CHECK(fingerprint(game) == saved);

  // Heard only while a jukebox stands; burnt, its song stops, and the
  // choice (gameplay) stays.
  CHECK(views::screens::restaurant::audible_jukebox_choice(r) == 3);
  for (const auto& pf : r.layout()->placed_furniture()) {
    if (pf->furniture()->role() == FurnitureRole::Jukebox) {
      pf->change_to_ashes();
    }
  }
  CHECK_FALSE(views::screens::restaurant::has_jukebox(r));
  CHECK(views::screens::restaurant::audible_jukebox_choice(r) == 0);
  CHECK(r.jukebox_music() == 3);
}
