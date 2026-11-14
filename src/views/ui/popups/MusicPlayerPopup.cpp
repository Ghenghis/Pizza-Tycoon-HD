/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/ui/popups/MusicPlayerPopup.h"

#include <algorithm>
#include <cmath>

#include "core/state/GameOptions.h"
#include "core/util/ConfigManager.h"
#include "engine/FormatCompat.h"
#include "engine/audio/AudioManager.h"
#include "engine/gfx/Draw.h"
#include "engine/input/InputManager.h"
#include "engine/music/MusicSystem.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/MusicFoldersPopup.h"

namespace {

using Type = views::ui::InteractionEvent::Type;

constexpr int pad = BasePopup::popup_border + 2;
constexpr int row_h = 22;
constexpr int buttons_y = 28;
constexpr ViewRect seek_rect{pad, 60, 200, 10};
constexpr ViewRect volume_rect{pad + 48, 76, 152, 10};
constexpr int options_y = 92;
constexpr int status_y = 120;
constexpr uint8_t text_color = 10;
constexpr float volume_step = 0.05f;
constexpr auto refresh_every = std::chrono::milliseconds(250);

std::string clock_text(double seconds) {
  const auto total = static_cast<long>(std::max(seconds, 0.0));
  return compat::format("{}:{:02}", total / 60, total % 60);
}

double fraction_at(const ViewRect& r, int x) {
  return std::clamp(static_cast<double>(x) / std::max(r.w - 1, 1), 0.0, 1.0);
}

std::string mode_label(music::Mode mode) {
  switch (mode) {
  case music::Mode::ORIGINAL:
    return "Original";
  case music::Mode::CHRISTMAS_BACKGROUND:
    return "Background";
  case music::Mode::JUKEBOX:
    return "Jukebox";
  case music::Mode::OFF:
    break;
  }
  return "Off";
}

std::string repeat_label(music::Repeat repeat) {
  switch (repeat) {
  case music::Repeat::OFF:
    return "Repeat: Off";
  case music::Repeat::ALL:
    return "Repeat: All";
  case music::Repeat::ONE:
    break;
  }
  return "Repeat: One";
}

} // namespace

MusicPlayerPopup::MusicPlayerPopup(const GameContext& ctx, GameOptions* options)
    : BasePopup(ctx), music_(ctx.music), audio_(ctx.audio), config_(ctx.config),
      popups_(ctx.popup), options_(options),
      volume_(options ? options->music_volume : 1.0f),
      prev_({pad, buttons_y, 26, row_h}, "|<",
            [this] { music_->playback().previous(); }),
      play_({pad + 30, buttons_y, 52, row_h}, "Play",
            [this] { play_pressed(); }),
      next_({pad + 86, buttons_y, 26, row_h}, ">|",
            [this] { music_->playback().next(); }),
      wide_({pad, buttons_y, 112, row_h}, "Play My Music",
            [this] { wide_pressed(); }),
      mode_({pad + 116, buttons_y, 100, row_h}, "Original",
            [this] { cycle_mode(); }),
      more_({pad + 220, buttons_y, 56, row_h}, "More",
            [this] { toggle_more(); }),
      close_({pad + 280, buttons_y, 26, row_h}, "X", [this] { finished(); }),
      repeat_({pad, options_y, 96, row_h}, "Repeat: All",
              [this] { cycle_repeat(); }),
      shuffle_({pad + 100, options_y, 96, row_h}, "Shuffle: Off",
               [this] {
                 music_->set_shuffle(!music_->playback().shuffle());
                 refresh();
               }),
      folders_({pad + 200, options_y, 106, row_h}, "Folders...",
               [this] { open_folders(); }) {
  prev_.id("music_prev");
  play_.id("music_play");
  next_.id("music_next");
  wide_.id("music_wide");
  mode_.id("music_mode");
  more_.id("music_more");
  close_.id("music_close");
  repeat_.id("music_repeat");
  shuffle_.id("music_shuffle");
  folders_.id("music_folders");

  prev_.tooltip("Previous track; past the first seconds it restarts this "
                "one (Page Up)");
  play_.tooltip("Play or pause your music (Enter)");
  next_.tooltip("Next track (Page Down)");
  mode_.tooltip("What plays: Original (the 1994 songs), Background (your "
                "music everywhere), Jukebox (your music at the North Pole "
                "jukebox) or Off. Click for the next one.");
  close_.tooltip("Close the player (Esc or Ctrl+M); the music plays on");
  repeat_.tooltip("Repeat the whole queue, this track, or play the queue "
                  "once");
  shuffle_.tooltip("Shuffle the tracks after the one playing");
  folders_.tooltip("Choose the folders your music is in");

  add_shortcut(input::Action::Exit, &close_);
  add_shortcut(input::Action::ToggleMusicPlayer, &close_);
  add_shortcut(input::Action::SubmitInput, &play_);
  add_shortcut(input::Action::PageUp, &prev_);
  add_shortcut(input::Action::PageDown, &next_);

  collapsed_ = music_->bar_collapsed();
  refresh();
}

Size MusicPlayerPopup::popup_size() const {
  return {width, collapsed_ ? collapsed_height : expanded_height};
}

bool MusicPlayerPopup::christmas() const {
  return status_.mode == music::Mode::CHRISTMAS_BACKGROUND ||
         status_.mode == music::Mode::JUKEBOX;
}

bool MusicPlayerPopup::transport() const {
  return christmas() && status_.queue_size > 0;
}

void MusicPlayerPopup::refresh() {
  status_ = music_->playback().status();
  notice_ = music_->last_notice();
  play_.text(status_.state == music::PlayState::PLAYING ? "Pause" : "Play");
  mode_.text(mode_label(status_.mode));
  more_.text(collapsed_ ? "More" : "Less");
  more_.tooltip(collapsed_ ? "Show the seek bar, volume, repeat, shuffle and "
                             "folders"
                           : "Show only the title and the buttons");
  repeat_.text(repeat_label(music_->playback().repeat()));
  shuffle_.text(music_->playback().shuffle() ? "Shuffle: On" : "Shuffle: Off");
  if (christmas()) {
    wide_.text("Find Music...");
    wide_.tooltip("No tracks found yet: choose the folders your music is "
                  "in");
  }
  else {
    wide_.text("Play My Music");
    wide_.tooltip("Play your own music everywhere (Background)");
  }
  next_refresh_ = std::chrono::steady_clock::now() + refresh_every;
  mark_dirty();
}

void MusicPlayerPopup::tick() {
  if (std::chrono::steady_clock::now() >= next_refresh_) {
    refresh();
  }
}

void MusicPlayerPopup::play_pressed() {
  if (!transport()) {
    wide_pressed();
    return;
  }
  music_->playback().toggle_pause();
  refresh();
}

void MusicPlayerPopup::wide_pressed() {
  if (christmas()) {
    open_folders();
    return;
  }
  music_->set_mode(music::Mode::CHRISTMAS_BACKGROUND);
  music_->playback().play();
  refresh();
}

void MusicPlayerPopup::cycle_mode() {
  switch (status_.mode) {
  case music::Mode::ORIGINAL:
    music_->set_mode(music::Mode::CHRISTMAS_BACKGROUND);
    break;
  case music::Mode::CHRISTMAS_BACKGROUND:
    music_->set_mode(music::Mode::JUKEBOX);
    break;
  case music::Mode::JUKEBOX:
    music_->set_mode(music::Mode::OFF);
    break;
  case music::Mode::OFF:
    music_->set_mode(music::Mode::ORIGINAL);
    break;
  }
  refresh();
}

void MusicPlayerPopup::toggle_more() {
  collapsed_ = !collapsed_;
  music_->set_bar_collapsed(collapsed_);
  refresh();
}

void MusicPlayerPopup::cycle_repeat() {
  switch (music_->playback().repeat()) {
  case music::Repeat::ALL:
    music_->set_repeat(music::Repeat::ONE);
    break;
  case music::Repeat::ONE:
    music_->set_repeat(music::Repeat::OFF);
    break;
  case music::Repeat::OFF:
    music_->set_repeat(music::Repeat::ALL);
    break;
  }
  refresh();
}

void MusicPlayerPopup::open_folders() {
  if (!popups_) {
    return;
  }
  // Queued until this popup is gone; closing the folders brings it back.
  PopupManager* popups = popups_;
  GameOptions* options = options_;
  popups_->create<MusicFoldersPopup>(
      [popups, options] { popups->create<MusicPlayerPopup>(options); });
  finished();
}

void MusicPlayerPopup::set_volume(float volume, bool save) {
  volume = std::clamp(volume, 0.0f, 1.0f);
  volume_ = volume;
  audio_->music_volume(volume);
  if (options_) {
    options_->music_volume = volume;
    if (save && config_) {
      config_->save_options(*options_);
    }
  }
  mark_dirty();
}

void MusicPlayerPopup::commit_seek(double fraction) {
  if (status_.duration_s > 0.0) {
    music_->playback().seek(fraction * status_.duration_s);
  }
  seek_drag_.reset();
  refresh();
}

bool MusicPlayerPopup::on_action(const input::ActionEvent& ev) {
  if (BasePopup::on_action(ev)) {
    return true;
  }
  switch (ev.action) {
  case input::Action::MoveLeft:
  case input::Action::MoveRight:
    if (ev.state.pressed && !is_finished()) {
      const float step =
          ev.action == input::Action::MoveLeft ? -volume_step : volume_step;
      set_volume(volume_ + step, true);
    }
    return true;
  case input::Action::ScrollLeft:
  case input::Action::ScrollRight:
    // The same keys as the volume; the map stays where it is.
    return true;
  default:
    return false;
  }
}

std::string MusicPlayerPopup::status_line() const {
  if (options_ && !options_->music) {
    return "Music Sound is off in Options - Audio";
  }
  const music::MusicLibrary::Progress p = music_->library().progress();
  if (p.scanning) {
    return compat::format("Looking for music: {} of {} files", p.files_done,
                          p.files_found);
  }
  if (!notice_.empty()) {
    return notice_;
  }
  if (christmas() && status_.queue_size == 0) {
    return "No tracks yet: Folders... chooses where your music is";
  }
  if (status_.queue_size > 0) {
    return compat::format("Track {} of {}", status_.queue_index + 1,
                          status_.queue_size);
  }
  return {};
}

void MusicPlayerPopup::draw_bar(Surface& s, ViewRect r, double fraction) const {
  s.fill_rect(r, 13);
  const int filled =
      static_cast<int>(std::lround(fraction * static_cast<double>(r.w - 2)));
  if (filled > 0) {
    s.fill_rect({r.x + 1, r.y + 1, filled, r.h - 2}, 10);
  }
  gfx::draw::box(s, r, 10, 1);
}

void MusicPlayerPopup::render(Surface& surface, ViewPoint origin) {
  const Size size = popup_size();
  auto s = popup_surface(size.w, size.h);
  const int inner_w = width - (2 * pad);

  std::string title;
  if (christmas() && !status_.title.empty()) {
    title = status_.artist.empty() ? status_.title
                                   : status_.artist + " - " + status_.title;
  }
  else if (christmas()) {
    title = "Your music";
  }
  else if (status_.mode == music::Mode::ORIGINAL) {
    title = "Original music: the game picks the songs";
  }
  else {
    title = "Music is off";
  }
  font_->render_line_onto(
      s, font_->truncate_to_width(title, FontFace::ASTANDAR, inner_w),
      {pad, pad + 1}, FontFace::ASTANDAR, text_color);

  if (transport()) {
    prev_.render_with(*this, s);
    play_.render_with(*this, s);
    next_.render_with(*this, s);
  }
  else {
    wide_.render_with(*this, s);
  }
  mode_.render_with(*this, s);
  more_.render_with(*this, s);
  close_.render_with(*this, s);

  if (!collapsed_) {
    if (transport()) {
      const double known = status_.duration_s;
      const double at = seek_drag_ ? *seek_drag_ * known : status_.position_s;
      draw_bar(s, seek_rect,
               known > 0.0 ? std::clamp(at / known, 0.0, 1.0) : 0.0);
      const std::string times =
          clock_text(at) + " / " +
          (known > 0.0 ? clock_text(known) + (status_.duration_exact ? "" : "?")
                       : std::string("--:--"));
      font_->render_line_onto(s, times, {seek_rect.right() + 6, seek_rect.y},
                              FontFace::PIZ_NUMB, text_color);
    }
    else {
      font_->render_line_onto(s, "No track to seek in", {pad, seek_rect.y},
                              FontFace::PIZ_NUMB, text_color);
    }

    font_->render_line_onto(s, "Volume", {pad, volume_rect.y},
                            FontFace::PIZ_NUMB, text_color);
    const float volume = volume_;
    draw_bar(s, volume_rect, static_cast<double>(volume));
    font_->render_line_onto(s,
                            compat::format("{}%", std::lround(volume * 100.0f)),
                            {volume_rect.right() + 6, volume_rect.y},
                            FontFace::PIZ_NUMB, text_color);

    repeat_.render_with(*this, s);
    shuffle_.render_with(*this, s);
    folders_.render_with(*this, s);

    font_->render_line_onto(
        s, font_->truncate_to_width(status_line(), FontFace::PIZ_NUMB, inner_w),
        {pad, status_y}, FontFace::PIZ_NUMB, text_color);
  }

  surface.blit_raw(s, origin);
}

void MusicPlayerPopup::add_interactions(views::ui::InteractionContext ictx,
                                        ViewPoint origin) {
  if (transport()) {
    prev_.add_interactions(ictx, origin);
    play_.add_interactions(ictx, origin);
    next_.add_interactions(ictx, origin);
  }
  else {
    wide_.add_interactions(ictx, origin);
  }
  mode_.add_interactions(ictx, origin);
  more_.add_interactions(ictx, origin);
  close_.add_interactions(ictx, origin);
  if (collapsed_) {
    return;
  }
  repeat_.add_interactions(ictx, origin);
  shuffle_.add_interactions(ictx, origin);
  folders_.add_interactions(ictx, origin);

  // Click or drag; the song moves when the button is let go.
  if (transport() && status_.duration_s > 0.0) {
    auto opts =
        PopupButton::tooltip_options("Click or drag to move in the track");
    opts.drag_button = views::ui::MouseButton::Left;
    ictx.layers.create_frame_item(
        add_origin(seek_rect, origin), "music_seek",
        [this, inv = ictx.invalidate](const views::ui::InteractionEvent& ev) {
          switch (ev.type) {
          case Type::ButtonDown:
          case Type::DragStart:
          case Type::DragMove:
            seek_drag_ = fraction_at(seek_rect, ev.local.x);
            inv();
            break;
          case Type::Click:
          case Type::DragEnd:
            commit_seek(fraction_at(seek_rect, ev.local.x));
            inv();
            break;
          case Type::Cancelled:
            seek_drag_.reset();
            inv();
            break;
          default:
            break;
          }
        },
        std::move(opts));
  }

  // Heard at once while it moves; saved when it is let go.
  auto opts = PopupButton::tooltip_options(
      "Music volume: click, drag or use the wheel (Left/Right keys)");
  opts.drag_button = views::ui::MouseButton::Left;
  ictx.layers.create_frame_item(
      add_origin(volume_rect, origin), "music_volume",
      [this, inv = ictx.invalidate](const views::ui::InteractionEvent& ev) {
        switch (ev.type) {
        case Type::ButtonDown:
        case Type::DragStart:
        case Type::DragMove:
          volume_held_ = true;
          set_volume(static_cast<float>(fraction_at(volume_rect, ev.local.x)),
                     false);
          inv();
          break;
        case Type::Click:
        case Type::DragEnd:
          volume_held_ = false;
          set_volume(static_cast<float>(fraction_at(volume_rect, ev.local.x)),
                     true);
          inv();
          break;
        case Type::Cancelled:
          if (volume_held_) {
            volume_held_ = false;
            set_volume(volume_, true);
          }
          inv();
          break;
        case Type::Scroll:
          if (ev.scroll_delta == 0) {
            break;
          }
          set_volume(volume_ +
                         (ev.scroll_delta > 0 ? volume_step : -volume_step),
                     true);
          inv();
          break;
        default:
          break;
        }
      },
      std::move(opts));
}
