/*
 * SPDX-FileCopyrightText: 2026 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "views/ui/popups/BasePopup.h"
#include "views/ui/popups/PopupButton.h"

struct GameOptions;

namespace audio {
class AudioManager;
}
namespace music {
class MusicSystem;
}
class Restaurant;

// The restaurant's jukebox (furniture JUKEBOX 138): the six original songs,
// which are gameplay (jukebox_music, saved; customers react to them). At the
// North Pole it also offers More..., the player's own music (the
// mini-player), which is cosmetic and never changes the choice here.
class JukeboxPopup : public BasePopup {
public:
  // north_pole and options (the music volume) for More....
  JukeboxPopup(const GameContext& ctx, Restaurant* restaurant,
               bool north_pole = false, GameOptions* options = nullptr);
  void add_interactions(views::ui::InteractionContext ictx,
                        ViewPoint origin) override;
  void render(Surface& surface, ViewPoint origin) override;

private:
  static ViewRect row_rect(int i);

  Restaurant* restaurant_;
  audio::AudioManager* audio_;
  music::MusicSystem* music_;
  int selected_music_; // 0 = OFF, 1-6 = music track index
  int hovered_row_{-1};
  bool north_pole_;
  GameOptions* options_;
  PopupManager* popups_;
  PopupButton more_;
  [[nodiscard]] bool offers_more() const;
};
