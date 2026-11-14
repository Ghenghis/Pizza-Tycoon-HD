/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "engine/music/MusicPlayback.h"
#include "views/ui/popups/BasePopup.h"
#include "views/ui/popups/PopupButton.h"

struct GameOptions;
class ConfigManager;
class PopupManager;

namespace audio {
class AudioManager;
}
namespace music {
class MusicSystem;
}

// The mini-player: what plays, play/pause, previous/next and the mode, with
// More for the seek bar, music volume, repeat, shuffle and the folders. The
// game runs on while it is open. Ctrl+M or Esc closes it; Enter is
// play/pause, Page Up/Down previous/next, Left/Right the volume.
class MusicPlayerPopup : public BasePopup {
public:
  // options holds the music volume, saved through ctx.config when it is
  // changed here. ctx.music must be set.
  MusicPlayerPopup(const GameContext& ctx, GameOptions* options);

  void tick() override;
  void render(Surface& surface, ViewPoint origin) override;
  void add_interactions(views::ui::InteractionContext ictx,
                        ViewPoint origin) override;
  Size popup_size() const override;
  bool keeps_game_running() const override { return true; }
  bool on_action(const input::ActionEvent& ev) override;

  static constexpr int width = 330;
  static constexpr int collapsed_height = 62;
  static constexpr int expanded_height = 140;

private:
  // Transport buttons show while the player's music has a queue; otherwise
  // one wide button offers the way there.
  [[nodiscard]] bool transport() const;
  [[nodiscard]] bool christmas() const;
  void refresh();
  void play_pressed();
  void wide_pressed();
  void cycle_mode();
  void toggle_more();
  void cycle_repeat();
  void open_folders();
  void set_volume(float volume, bool save);
  void commit_seek(double fraction);
  [[nodiscard]] std::string status_line() const;
  void draw_bar(Surface& s, ViewRect r, double fraction) const;

  music::MusicSystem* music_;
  audio::AudioManager* audio_;
  ConfigManager* config_;
  PopupManager* popups_;
  GameOptions* options_;
  float volume_; // the music volume, as options_ has it

  music::MusicPlayback::Status status_;
  std::string notice_;
  bool collapsed_ = true;
  std::chrono::steady_clock::time_point next_refresh_{};
  std::optional<double> seek_drag_; // 0..1 while the seek bar is held
  bool volume_held_ = false;

  PopupButton prev_;
  PopupButton play_;
  PopupButton next_;
  PopupButton wide_;
  PopupButton mode_;
  PopupButton more_;
  PopupButton close_;
  PopupButton repeat_;
  PopupButton shuffle_;
  PopupButton folders_;
};
