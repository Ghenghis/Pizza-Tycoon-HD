/*
 * SPDX-FileCopyrightText: 2010 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <string>
#include "core/state/UIState.h"
#include "engine/audio/AudioManager.h"
#include "engine/music/MusicTypes.h"
#include "views/base/Menu.h"

// The HD render scale choices on the graphics options screen (the value is
// GameOptions::hd_scale).
enum class HdResolution : int {
  Auto = 0,
  Off = 1,
  X2 = 2,
  X3 = 3,
  X4 = 4,
  X5 = 5,
  X6 = 6,
  X8 = 8,
};
[[nodiscard]] std::string to_string(HdResolution r);

class MenuOptions : public Menu {
public:
  // Main choice screen
  MenuOptions(GameContext& context, GameState& state);
  // Settings sub-screen
  MenuOptions(GameContext& context, GameState& state, OptionsScreen screen,
              bool focus_advanced_toggle = false);

protected:
  Surface background() const override;

private:
  void build_chooser(GameState& state);
  void build_audio(GameState& state);
  void build_graphics(GameState& state);
  void build_controls(GameState& state);
  void build_gameplay(GameState& state);
  void build_rules(GameState& state);
  // Returns the advanced options checkbox, if the screen has one.
  MenuItem* add_save_and_back(GameState& state, OptionsScreen screen);
  void add_back(std::function<void()> action);

  std::string title_;
  HdResolution hd_resolution_ = HdResolution::Auto;
  // What the audio screen's choices show.
  music::Mode music_mode_ = music::Mode::ORIGINAL;
  audio::EffectsSet effects_set_ = audio::EffectsSet::ORIGINAL;
};
