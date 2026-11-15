/*
 * SPDX-FileCopyrightText: 2026 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/ui/popups/JukeboxPopup.h"

#include <array>

#include "core/restaurant/Restaurant.h"
#include "engine/audio/AudioManager.h"
#include "engine/geom/Spaces.h"
#include "engine/gfx/Draw.h"
#include "engine/music/MusicSystem.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/MusicPlayerPopup.h"

static constexpr std::array<const char*, 7> music_titles = {
    "-------- OFF --------",
    "Tech The Tronic",
    "Jam The Scratch",
    "Heavy Banger",
    "Amore Mio",
    "Elevator Music",
    "Tea Shoppe",
};

// Popup-local rect for row i (0=OFF, 1-6=music tracks).
ViewRect JukeboxPopup::row_rect(int i) { return {46, 44 + (i * 20), 220, 22}; }

JukeboxPopup::JukeboxPopup(const GameContext& ctx, Restaurant* restaurant,
                           bool north_pole, GameOptions* options)
    : BasePopup(ctx), restaurant_(restaurant), audio_(ctx.audio),
      music_(ctx.music), selected_music_(restaurant->jukebox_music()),
      north_pole_(north_pole), options_(options), popups_(ctx.popup),
      more_({196, 194, 76, 22}, "More...", [this] {
        // The player's own music; the song chosen here stays as it is.
        PopupManager* popups = popups_;
        GameOptions* options = options_;
        popups->create<MusicPlayerPopup>(options);
        finished();
      }) {
  more_.id("jukebox_more");
  more_.tooltip("Your own music: folders, collections and playlists, in the "
                "music player. The song chosen here plays on for the guests.");
}

bool JukeboxPopup::offers_more() const {
  return north_pole_ && music_ && popups_;
}

void JukeboxPopup::add_interactions(views::ui::InteractionContext ictx,
                                    ViewPoint origin) {
  for (int i = 0; i < 7; ++i) {
    ictx.layers.create_frame_item(
        add_origin(row_rect(i), origin), compat::format("jukebox_row_{}", i),
        [this, i,
         inv = ictx.invalidate](const views::ui::InteractionEvent& ev) {
          switch (ev.type) {
          case views::ui::InteractionEvent::Type::Enter:
            hovered_row_ = i;
            inv();
            break;

          case views::ui::InteractionEvent::Type::Leave:
            if (hovered_row_ == i) {
              hovered_row_ = -1;
              inv();
            }
            break;

          case views::ui::InteractionEvent::Type::Click:
            restaurant_->jukebox_music(i);
            if (music_) {
              // The music owner makes the same FM call, and never touches
              // the player's music.
              music_->playback().on_restaurant_jukebox(i);
            }
            else if (i == 0) {
              if (audio_) {
                audio_->stop_music();
              }
            }
            else {
              if (audio_) {
                audio_->play_music(static_cast<audio::MusicId>(i - 1));
              }
            }
            finished();
            break;

          default:
            break;
          }
        });
  }
  if (offers_more()) {
    more_.add_interactions(ictx, origin);
  }
}

void JukeboxPopup::render(Surface& surface, ViewPoint origin) {
  const Size box_size{288, 226};
  const ViewRect outer{origin, box_size};
  surface.fill_rect(outer, 15);
  const int c1 = 17;
  const int c2 = 16;
  const int c3 = 18;
  gfx::draw::box(surface, outer.inset(0, 0), c1, 2);
  gfx::draw::box(surface, outer.inset(2, 2), c2, 2);
  gfx::draw::box(surface, outer.inset(4, 4), c1, 2);
  gfx::draw::box(surface, outer.inset(6, 6), c3, 2);

  const PxOffset title_offset{box_size.w / 2, 18};
  font_->render_line_onto(
      surface, north_pole_ ? "North Pole Jukebox" : "Music Selection",
      origin + title_offset, FontFace::ASTANDAR, 240, TextAnchor::Middle);

  for (int i = 0; i < static_cast<int>(music_titles.size()); ++i) {
    const ViewRect local = row_rect(i);
    const ViewRect screen_row = add_origin(local, origin);
    const bool selected = (i == selected_music_);
    const bool hovered = (i == hovered_row_);

    if (selected) {
      gfx::draw::box(surface, screen_row, 20, 1);
    }
    if (hovered) {
      surface.fill_rect(screen_row.inset(2, 2), 13);
    }

    const uint8_t text_color = 240;
    font_->render_line_onto(surface, music_titles[i],
                            origin + PxOffset{local.x + 4, local.y + 4},
                            FontFace::PIZ_NUMB, text_color);
  }

  const PxOffset please_select_offset{50, 200};
  font_->render_line_onto(surface, "Please select...",
                          origin + please_select_offset, FontFace::PIZ_NUMB,
                          14);
  if (offers_more()) {
    // Drawn on its own surface, as the kit's buttons are, then placed.
    auto button = text_button("More...", 76, 22, false);
    surface.blit_raw(button, origin + PxOffset{196, 194});
  }
}
