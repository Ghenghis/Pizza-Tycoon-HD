/*
 * SPDX-FileCopyrightText: 2024 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/menus/MenuOptions.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/food/PizzaJudging.h"
#include "core/state/GameOptions.h"
#include "core/state/GameRules.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "core/util/ConfigManager.h"
#include "engine/DisplayConstants.h"
#include "engine/Logger.h"
#include "engine/audio/AudioManager.h"
#include "engine/fx/Look.h"
#include "engine/input/Action.h"
#include "engine/input/InputManager.h"
#include "engine/input/Key.h"
#include "engine/music/MusicSystem.h"
#include "resources/FontTypes.h"
#include "resources/HdTypeface.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/AlertPopup.h"
#include "views/ui/popups/MusicFoldersPopup.h"
#include "views/ui/popups/MusicPlayerPopup.h"
#include "views/ui/widgets/CheckboxMenuItem.h"
#include "views/ui/widgets/EnumMenuItem.h"
#include "views/ui/widgets/IntSliderMenuItem.h"
#include "views/ui/widgets/LabelMenuItem.h"
#include "views/ui/widgets/PositionMarkerMenuItem.h"
#include "views/ui/widgets/SliderMenuItem.h"
#include "views/ui/widgets/SpacerMenuItem.h"
#include "views/ui/widgets/TextMenuItem.h"
#include "views/ui/widgets/ToggleMenuItem.h"

namespace {
constexpr FontFace face = FontFace::PIZ_NUMB;
const TextStyle style{
    .color = 240,
};
const TextStyle sel_style{
    .color = 29,
};
const TextStyle label_style{
    .color = 240,
    .shadow = ShadowStyle{.offset = {-1, 1}, .color = 246},
};
constexpr int label_gap = 6;
// A blank row before "Save rules for new games".
constexpr int row_gap = 14;

// "Ctrl+F or F11", or empty when nothing is bound.
std::string chord_list(const std::vector<input::Binding>& chords) {
  std::string joined;
  for (const input::Binding& chord : chords) {
    if (!joined.empty()) {
      joined += " or ";
    }
    joined += input::chord_to_string(chord);
  }
  return joined;
}
} // anonymous namespace

MenuOptions::MenuOptions(GameContext& context, GameState& state)
    : Menu(context, state, ViewPoint{120, 140}, 12), title_("Options") {
  build_chooser(state);
  update_selection(nullptr);
}

MenuOptions::MenuOptions(GameContext& context, GameState& state,
                         OptionsScreen screen, bool focus_advanced_toggle)
    : Menu(context, state, ViewPoint{120, 140}, 3) {
  switch (screen) {
  case OptionsScreen::Audio:
    title_ = "Options - Audio";
    build_audio(state);
    break;
  case OptionsScreen::Graphics:
    title_ = "Options - Graphics";
    build_graphics(state);
    break;
  case OptionsScreen::Controls:
    title_ = "Options - Controls";
    build_controls(state);
    break;
  case OptionsScreen::Gameplay:
    title_ = "Options - Gameplay";
    build_gameplay(state);
    break;
  case OptionsScreen::Rules:
    title_ = "Options - Rules";
    build_rules(state);
    break;
  }
  MenuItem* advanced_toggle = add_save_and_back(state, screen);
  update_selection(focus_advanced_toggle ? advanced_toggle : nullptr);
}

void MenuOptions::build_chooser(GameState& state) {
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "> Audio",
      [&state] { state.ui().push<OptionsMenuState>(OptionsScreen::Audio); },
      face, style, sel_style));
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "> Graphics",
      [&state] { state.ui().push<OptionsMenuState>(OptionsScreen::Graphics); },
      face, style, sel_style));
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "> Controls",
      [&state] { state.ui().push<OptionsMenuState>(OptionsScreen::Controls); },
      face, style, sel_style));
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "> Gameplay",
      [&state] { state.ui().push<OptionsMenuState>(OptionsScreen::Gameplay); },
      face, style, sel_style));
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "> Rules",
      [&state] { state.ui().push<OptionsMenuState>(OptionsScreen::Rules); },
      face, style, sel_style));

  // Sub-screens save their own changes when leaving, so from the main screen we
  // can just pop.
  add_back([&state] { state.ui().pop(); });
}

void MenuOptions::add_back(std::function<void()> action) {
  // The bottom right corner, where the other menus have theirs
  constexpr int back_x = 426;
  constexpr int back_y = 230;
  add_menu_item(
      std::make_unique<PositionMarkerMenuItem>(context_, back_x, back_y));
  add_menu_item(std::make_unique<TextMenuItem>(
      context_, "Back", std::move(action), FontFace::RUBY, style, sel_style));
}

void MenuOptions::build_audio(GameState& state) {
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Map FX", state.options().map_fx,
                    [&state, this] {
                      state.options().map_fx = !state.options().map_fx;
                      mark_dirty();
                    }))
      .help_text("Background sounds on the city map.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Restaurant FX", state.options().restaurant_fx,
                    [&state, this] {
                      state.options().restaurant_fx =
                          !state.options().restaurant_fx;
                      mark_dirty();
                    }))
      .help_text("Background sounds inside a restaurant.");
  if (context_.music) {
    // What plays, live: the game goes on, effects are untouched, and the
    // FM song is where it was when Original comes back. Saved in the music
    // settings, not here.
    music_mode_ = context_.music->mode();
    add_menu_item(std::make_unique<EnumMenuItem<music::Mode>>(
                      context_, "Music", music_mode_,
                      std::vector{music::Mode::ORIGINAL,
                                  music::Mode::CHRISTMAS_BACKGROUND,
                                  music::Mode::JUKEBOX, music::Mode::OFF},
                      [this] {
                        context_.music->set_mode(music_mode_);
                        mark_dirty();
                      },
                      face))
        .help_text("Original plays the 1994 songs; Christmas Background and "
                   "Jukebox play your own music; Off plays none. Effects stay "
                   "as they are.");
    add_menu_item(std::make_unique<TextMenuItem>(
                      context_, "Music Player",
                      [&state, this] {
                        context_.popup->create<MusicPlayerPopup>(
                            &state.options());
                      },
                      face))
        .help_text("Play, pause, skip and seek your music, with its volume, "
                   "repeat and shuffle. Ctrl+M opens it anywhere.");
    add_menu_item(std::make_unique<TextMenuItem>(
                      context_, "Music Folders",
                      [this] { context_.popup->create<MusicFoldersPopup>(); },
                      face))
        .help_text("The folders your music is in: add, rename, locate, turn "
                   "off or remove them. Nothing changes until Save.");
  }
  effects_set_ = context_.audio->effects();
  add_menu_item(
      std::make_unique<EnumMenuItem<audio::EffectsSet>>(
          context_, "Sound Effects", effects_set_,
          std::vector{audio::EffectsSet::ORIGINAL, audio::EffectsSet::ENHANCED},
          [&state, this] {
            if (const auto done = context_.audio->select_effects(effects_set_);
                !done) {
              Logger::warn("Enhanced effects not used: {}", done.error);
              context_.popup->create<AlertPopup>(
                  "The enhanced sound effects could not be used, so the "
                  "original ones play. " +
                  done.error);
            }
            effects_set_ = context_.audio->effects();
            state.options().enhanced_effects =
                effects_set_ == audio::EffectsSet::ENHANCED;
            mark_dirty();
          },
          face))
      .help_text("Enhanced plays the remaster's sound pack where it has a "
                 "sound; every other effect stays the original.");
  add_menu_item(std::make_unique<TextMenuItem>(
                    context_, "Restore Original Audio",
                    [&state, this] {
                      if (context_.music) {
                        context_.music->restore_original();
                        music_mode_ = context_.music->mode();
                      }
                      static_cast<void>(context_.audio->select_effects(
                          audio::EffectsSet::ORIGINAL));
                      effects_set_ = context_.audio->effects();
                      state.options().enhanced_effects = false;
                      state.options().music = true;
                      context_.audio->music_enabled(true);
                      mark_dirty();
                    },
                    face))
      .help_text("The 1994 music and effects, at once and without a restart. "
                 "Your music folders and playlists stay.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Music Sound", state.options().music,
                    [&state, this] {
                      state.options().music = !state.options().music;
                      context_.audio->music_enabled(state.options().music);
                      mark_dirty();
                    }))
      .help_text("Silences the music, which plays on underneath; Off above "
                 "stops it.");
  add_menu_item(std::make_unique<SliderMenuItem>(
                    context_, "Sound Volume", state.options().sound_volume,
                    [this](float v) { context_.audio->sound_volume(v); }))
      .help_text("The sound effects.");
  add_menu_item(std::make_unique<SliderMenuItem>(
                    context_, "Music Volume", state.options().music_volume,
                    [this](float v) { context_.audio->music_volume(v); }))
      .help_text("The music, original or your own.");
  add_menu_item(std::make_unique<SliderMenuItem>(
                    context_, "Master Volume", state.options().master_volume,
                    [this](float v) { context_.audio->master_volume(v); }))
      .help_text("Music and sound effects together.");
  add_menu_item(
      std::make_unique<ToggleMenuItem>(
          context_, "Mute in Background", state.options().mute_unfocused,
          [&state, this] {
            state.options().mute_unfocused = !state.options().mute_unfocused;
            context_.audio->mute_when_unfocused(state.options().mute_unfocused);
            mark_dirty();
          }))
      .help_text("Silence the game while another window is in front.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Music Ducking", state.options().duck_music,
                    [&state, this] {
                      state.options().duck_music = !state.options().duck_music;
                      context_.audio->ducking(state.options().duck_music);
                      mark_dirty();
                    }))
      .help_text("The music, and the enhanced pack's room sounds, dip a little "
                 "while the phone rings or the police come, and come back "
                 "smoothly.");
}

std::string to_string(HdResolution r) {
  switch (r) {
  case HdResolution::Auto:
    return "Auto";
  case HdResolution::Off:
    return "Off (640x400)";
  default: {
    const int k = static_cast<int>(r);
    return std::to_string(DisplayConstants::SCREEN_WIDTH * k) + "x" +
           std::to_string(DisplayConstants::SCREEN_HEIGHT * k);
  }
  }
}

void MenuOptions::build_graphics(GameState& state) {
  hd_resolution_ = static_cast<HdResolution>(state.options().hd_scale);
  add_menu_item(
      std::make_unique<EnumMenuItem<HdResolution>>(
          context_, "HD resolution", hd_resolution_,
          std::vector{HdResolution::Auto, HdResolution::Off, HdResolution::X2,
                      HdResolution::X3, HdResolution::X4, HdResolution::X5,
                      HdResolution::X6, HdResolution::X8},
          [&state, this] {
            state.options().hd_scale = static_cast<int>(hd_resolution_);
            mark_dirty();
          },
          face))
      .help_text("How sharp the remaster draws: Auto fits your screen. "
                 "F7 shows the 1994 look, F8 both side by side. Takes effect "
                 "after a restart.");
  add_menu_item(
      std::make_unique<ToggleMenuItem>(
          context_, "CRT Aspect Ratio (4:3)", state.options().crt_aspect_ratio,
          [&state, this] {
            state.options().crt_aspect_ratio =
                !state.options().crt_aspect_ratio;
            state.request_crt_change(state.options().crt_aspect_ratio);
            mark_dirty();
          }))
      .help_text("The game draws 640x400, and a nineties monitor stretched "
                 "that to 4:3; this does the same.");
  add_menu_item(
      std::make_unique<ToggleMenuItem>(
          context_, "Double Size", state.options().double_size,
          [&state, this] {
            state.options().double_size = !state.options().double_size;
            state.request_double_size_change(state.options().double_size);
            mark_dirty();
          }))
      .help_text("Show the window at twice the size. You can also just resize "
                 "it by grabbing a corner.");
  MenuItem& fullscreen = add_menu_item(std::make_unique<ToggleMenuItem>(
      context_, "Fullscreen", state.options().fullscreen, [&state, this] {
        state.options().fullscreen = !state.options().fullscreen;
        state.request_fullscreen_change(state.options().fullscreen);
        mark_dirty();
      }));
  if (const std::string keys = chord_list(
          context_.input->button_bindings_for(input::Action::ToggleFullscreen));
      !keys.empty()) {
    fullscreen.help_text("You can also toggle it any time with " + keys + ".");
  }
  add_menu_item(
      std::make_unique<EnumMenuItem<FpsOption>>(
          context_, "Render FPS", state.options().render_fps,
          std::vector{FpsOption::Fps30, FpsOption::Fps60, FpsOption::Fps120},
          [this] { mark_dirty(); }, face))
      .help_text("How often the screen is redrawn. The game itself animates "
                 "30 times a second at normal speed, so a higher number "
                 "mainly smooths the cursor.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "VSync", state.options().vsync,
                    [&state, this] {
                      state.options().vsync = !state.options().vsync;
                      mark_dirty();
                    }))
      .help_text("Synchronize drawing with the screen's refresh rate to prevent"
                 " tearing.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Smooth motion", state.options().smooth_motion,
                    [&state, this] {
                      state.options().smooth_motion =
                          !state.options().smooth_motion;
                      mark_dirty();
                    }))
      .help_text("Walking guests and waiters glide from step to step instead "
                 "of jumping. Only the picture changes, not the game.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Lighting", state.options().lighting,
                    [&state, this] {
                      state.options().lighting = !state.options().lighting;
                      mark_dirty();
                    }))
      .help_text("HD only: candles, ovens, arcade machines and TVs light the "
                 "room around them. Only the picture changes, not the game.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "HD text", state.options().hd_text,
                    [&state, this] {
                      state.options().hd_text = !state.options().hd_text;
                      resources::HdTypefaces::set_enabled(
                          state.options().hd_text);
                      mark_dirty();
                    }))
      .help_text("HD only: menus and reports in sharp modern typefaces, "
                 "drawn at full resolution, instead of the 1994 pixel fonts.");
  add_menu_item(std::make_unique<EnumMenuItem<Smoothing>>(
                    context_, "Smoothing", state.options().smoothing,
                    std::vector{Smoothing::Off, Smoothing::Text,
                                Smoothing::TextAndGraphics},
                    [this] { mark_dirty(); }, face))
      .help_text("Round off the blocky edges of letters, or of letters and "
                 "pictures. Takes effect after a restart.");

  // Picture: a preset like a TV's picture modes, then the sliders on top of
  // it (More). Changes show at once.
  fx::LookSettings& look = state.options().picture;
  auto changed = [this, &look] {
    fx::set_frame_look(look, fx::Area::Other);
    mark_dirty();
  };
  auto changed_int = [changed](int) { changed(); };
  constexpr const char* gpu_only =
      " Needs the GPU renderer (Renderer: GPU (SDL GPU)).";

  add_menu_item(std::make_unique<SpacerMenuItem>(context_, row_gap));
  add_menu_item(
      std::make_unique<LabelMenuItem>(context_, "Picture", face, label_style));
  add_menu_item(std::make_unique<SpacerMenuItem>(context_, label_gap));
  add_menu_item(std::make_unique<EnumMenuItem<fx::Preset>>(
                    context_, "Preset", look.preset,
                    std::vector<fx::Preset>(fx::all_presets.begin(),
                                            fx::all_presets.end()),
                    changed, face))
      .help_text("HD only, like a TV's picture modes. Classic: the HD look as "
                 "it always was. Natural: a touch of contrast, warmth and "
                 "glow. Vivid: rich colour, bright lights. Cinematic: warm "
                 "highlights, deep shadows, glow and dark corners. Studio: "
                 "neutral, with open shadows. Only the picture changes.");
  add_menu_item(
      std::make_unique<EnumMenuItem<fx::RendererChoice>>(
          context_, "Renderer", state.options().renderer,
          std::vector{fx::RendererChoice::Auto, fx::RendererChoice::Gpu},
          [this] { mark_dirty(); }, face))
      .help_text("Auto lets SDL choose (Direct3D 11 on Windows). GPU uses "
                 "SDL's GPU renderer (Direct3D 12, Vulkan or Metal), which "
                 "runs the full picture pass: bloom, vignette, CRT and the "
                 "complete colour grade. If it cannot start, the game uses "
                 "Auto. Takes effect after a restart.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "HDR output", state.options().hdr_output,
                    [&state, this] {
                      state.options().hdr_output = !state.options().hdr_output;
                      mark_dirty();
                    }))
      .help_text("Send the picture to an HDR display in its own colour space "
                 "(scRGB), so glow can shine above paper white. Only used "
                 "when Windows HDR is on. Takes effect after a restart.");

  add_menu_item(std::make_unique<SpacerMenuItem>(context_, row_gap));
  add_menu_item(std::make_unique<LabelMenuItem>(context_, "Picture - More",
                                                face, label_style));
  add_menu_item(std::make_unique<SpacerMenuItem>(context_, label_gap));
  fx::Sliders& sliders = look.sliders;
  auto slider = [this, &changed_int](const std::string& label, int& value,
                                     const std::string& help) {
    add_menu_item(std::make_unique<IntSliderMenuItem>(
                      context_, label, value, fx::Sliders::MIN,
                      fx::Sliders::MAX, changed_int, face, 5))
        .help_text(help);
  };
  slider("Light intensity", sliders.intensity,
         "Brighter or darker picture, and stronger or weaker candles, ovens "
         "and lamps. 0 is as the preset has it.");
  slider("Shadow depth", sliders.shadows,
         "How dark the rooms get away from their lights, and how deep the "
         "blacks go. 0 is as the preset has it.");
  slider("Bloom", sliders.bloom,
         std::string("Glow around bright things: flames, screens, sunlit "
                     "walls. 0 is as the preset has it.") +
             gpu_only);
  slider("Colour warmth", sliders.warmth,
         "White balance: up for warmer, amber light; down for cooler, bluer "
         "light. 0 is as the preset has it.");
  slider("Vignette", sliders.vignette,
         std::string("Darker corners that draw the eye to the middle. 0 is "
                     "as the preset has it.") +
             gpu_only);
  add_menu_item(std::make_unique<ToggleMenuItem>(context_, "CRT look",
                                                 sliders.crt,
                                                 [&sliders, changed] {
                                                   sliders.crt = !sliders.crt;
                                                   changed();
                                                 }))
      .help_text(std::string("Scanlines between the original rows and a soft "
                             "aperture mask, like a nineties monitor.") +
                 gpu_only);
  for (const fx::Area area : fx::overridable_areas) {
    add_menu_item(std::make_unique<EnumMenuItem<fx::AreaPreset>>(
                      context_, fx::to_string(area) + " preset",
                      look.per_area[area],
                      std::vector<fx::AreaPreset>(fx::all_area_presets.begin(),
                                                  fx::all_area_presets.end()),
                      changed, face))
        .help_text("A preset of its own for the " + fx::to_string(area) +
                   ", or the same as Preset above. The sliders apply on top "
                   "of it.");
  }
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Day and night", look.day_night,
                    [&look, changed] {
                      look.day_night = !look.day_night;
                      changed();
                    }))
      .help_text("HD only: the game clock colours the city map through the "
                 "day (dawn, day, dusk, night) and lights its street lamps "
                 "at night. Not in the Classic preset. (Lit windows, signs and "
                 "ovens glow at night in every preset but Classic.) Only the "
                 "picture changes, not the game.");
  for (const fx::Area area : fx::overridable_areas) {
    add_menu_item(std::make_unique<EnumMenuItem<fx::Weather>>(
                      context_, fx::to_string(area) + " weather",
                      look.weather[area],
                      std::vector<fx::Weather>(fx::all_weather.begin(),
                                               fx::all_weather.end()),
                      changed, face))
        .help_text("HD only: snow falling over the " + fx::to_string(area) +
                   ", for a winter look. Only the picture changes, not the "
                   "game.");
  }
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Reduced motion", look.reduced_motion,
                    [&look, changed] {
                      look.reduced_motion = !look.reduced_motion;
                      changed();
                    }))
      .help_text("Fewer snowflakes, falling slowly and straight down.");
  add_menu_item(
      std::make_unique<EnumMenuItem<fx::AmbientMotion>>(
          context_, "Ambient motion", look.ambient_motion,
          std::vector<fx::AmbientMotion>(fx::all_ambient_motion.begin(),
                                         fx::all_ambient_motion.end()),
          changed, face))
      .help_text("HD only, at the North Pole: the aurora over the city map, "
                 "twinkling windows and falling snow. Full: all of it. "
                 "Reduced: slow, no twinkle. Static: still. Off: none. "
                 "Reduced motion above caps it at Reduced.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Music accent", look.music_accent,
                    [&look, changed] {
                      look.music_accent = !look.music_accent;
                      changed();
                    }))
      .help_text("At the North Pole, lit windows pulse a little with the "
                 "music. Only with Ambient motion Full.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Screenshots as shown", look.graded_screenshots,
                    [&look, changed] {
                      look.graded_screenshots = !look.graded_screenshots;
                      changed();
                    }))
      .help_text("On: screenshots show the picture as you see it, with the "
                 "preset, day and night and the weather. Off: the picture "
                 "underneath, as the 1994 comparison uses it.");
}

void MenuOptions::build_gameplay(GameState& state) {
  auto& options = state.options();
  auto toggle = [this](const std::string& label, bool& value) -> MenuItem& {
    return add_menu_item(std::make_unique<ToggleMenuItem>(
        context_, label, value, [&value, this] {
          value = !value;
          mark_dirty();
        }));
  };

  toggle("Automatic Report", options.automatic_report)
      .help_text("Automatically show the daily turnover report at the end of "
                 "the turn.");
  toggle("Quality of Life improvements", options.qol_improvements)
      .help_text("Enable various modernizations of the UI and mouse behavior.");
  add_menu_item(
      std::make_unique<EnumMenuItem<AutosaveFrequency>>(
          context_, "Auto save", options.autosave_frequency,
          std::vector{AutosaveFrequency::Off, AutosaveFrequency::Daily,
                      AutosaveFrequency::Weekly, AutosaveFrequency::Monthly},
          [this] { mark_dirty(); }, face))
      .help_text("Automatically save the game at the end of the day, week or "
                 "month.");
  add_menu_item(std::make_unique<IntSliderMenuItem>(
      context_, "Auto save slots", options.autosave_count,
      GameOptions::AUTOSAVE_COUNT_MIN, GameOptions::AUTOSAVE_COUNT_MAX,
      [this](int) { mark_dirty(); }));

  if (!options.show_advanced_options) {
    return;
  }
  add_menu_item(std::make_unique<IntSliderMenuItem>(
                    context_, "Attack scene speed", options.attack_scene_hz,
                    GameOptions::ATTACK_SCENE_HZ_MIN,
                    GameOptions::ATTACK_SCENE_HZ_MAX,
                    [this](int) { mark_dirty(); }, face, 5))
      .advanced(true)
      .help_text("Updates per second in burglaries. The original ran at 70 "
                 "on a fast 486 and 35 or less on slower machines.");
  toggle("Player colors in dropdowns", options.show_player_color_in_dropdown)
      .advanced(true)
      .help_text("On: the Player and Range lists on the city map show each "
                 "player's color.");
}

void MenuOptions::build_rules(GameState& state) {
  // From the main menu the rules are the ones a new game starts with; from
  // the disk menu they are the ones from the running game.
  const bool in_game = state.ui().contains<CityViewState>();
  GameRules& rules = in_game ? state.rules() : state.options().rules;
  add_menu_item(std::make_unique<LabelMenuItem>(
      context_, in_game ? "Rules for this game" : "Rules for new games", face,
      label_style));
  add_menu_item(std::make_unique<SpacerMenuItem>(context_, label_gap));
  auto changed = [this, &state, in_game] {
    if (in_game) {
      state.rules_changed(true);
    }
    mark_dirty();
  };
  auto toggle = [this, changed](const std::string& label,
                                bool& value) -> MenuItem& {
    return add_menu_item(std::make_unique<ToggleMenuItem>(
        context_, label, value, [&value, changed] {
          value = !value;
          changed();
        }));
  };

  toggle(rule_labels::fix_legacy_bugs, rules.fix_legacy_bugs)
      .help_text("Fix various bugs from the original; see "
                 "docs/MODERNIZATIONS.md for details.");
  add_menu_item(std::make_unique<EnumMenuItem<PizzaJudging>>(
                    context_, rule_labels::pizza_judging, rules.pizza_judging,
                    std::vector{PizzaJudging::Original, PizzaJudging::Relaxed},
                    changed, face))
      .help_text("Original: match the cook book exactly. "
                 "Relaxed: the right ingredients in the right amounts.");
  toggle(rule_labels::cook_book, rules.cook_book)
      .help_text("On: a right click on a standard pizza offers to show its "
                 "cook book entry. Off: the entry is not shown.");
  if (state.options().show_advanced_options) {
    toggle(rule_labels::mafia_shakedowns_target_all_restaurants,
           rules.mafia_shakedowns_target_all_restaurants)
        .advanced(true)
        .help_text("On: the mafia can shake down any of your restaurants. Off: "
                   "only your first one, as in the original.");
    toggle(rule_labels::demand_fulfillment_popularity,
           rules.demand_fulfillment_popularity)
        .advanced(true)
        .help_text(
            "On: popularity reflects unmet orders. "
            "Off: it always takes the full drop, as in the DOS original.");
    toggle(rule_labels::suppliers_remember_missed_payments,
           rules.suppliers_remember_missed_payments)
        .advanced(true)
        .help_text("On: after a missed contract payment a supplier won't halve "
                   "the penalty or give extra discount for a while. Off: they "
                   "forget, as in the original.");
    add_menu_item(std::make_unique<EnumMenuItem<WinCondition>>(
                      context_, rule_labels::win_condition, rules.win_condition,
                      std::vector{WinCondition::Paris, WinCondition::AnyCity,
                                  WinCondition::AllCities},
                      changed, face))
        .advanced(true)
        .help_text("Paris: win with enough market share in Paris. "
                   "Any: win in any city. All: win in every city.");
  }
  if (in_game) {
    add_menu_item(std::make_unique<SpacerMenuItem>(context_, row_gap));
    add_menu_item(std::make_unique<TextMenuItem>(
                      context_, "Save rules for new games",
                      [&state, this] {
                        state.options().rules = state.rules();
                        context_.config->save_options(state.options());
                        context_.popup->create<AlertPopup>(
                            "New games will start with these rules.");
                      },
                      face, style, sel_style))
        .help_text("Make the rules used in the current game the rules for "
                   "new games.");
  }
}

void MenuOptions::build_controls(GameState& state) {
  add_menu_item(std::make_unique<TextMenuItem>(
                    context_, "> Key bindings",
                    [&state] {
                      state.ui().push<ScreenTag>(ScreenTag::Kind::MenuBindings);
                    },
                    face, style, sel_style))
      .help_text("Choose the keys and gamepad buttons for each action.");
  add_menu_item(
      std::make_unique<ToggleMenuItem>(context_, "Invert map scrolling",
                                       state.options().rmb_drag_inverted,
                                       [&state, this] {
                                         state.options().rmb_drag_inverted =
                                             !state.options().rmb_drag_inverted;
                                         mark_dirty();
                                       }))
      .help_text("Which way the map moves when you drag it with the right "
                 "mouse button.");
  add_menu_item(std::make_unique<ToggleMenuItem>(
                    context_, "Invert scroll wheel direction",
                    state.options().scroll_wheel_inverted,
                    [&state, this] {
                      state.options().scroll_wheel_inverted =
                          !state.options().scroll_wheel_inverted;
                      mark_dirty();
                    }))
      .help_text("Reverse the direction of the scroll wheel.");
  add_menu_item(
      std::make_unique<ToggleMenuItem>(
          context_, "Use software cursor", state.options().software_cursor,
          [&state, this] {
            state.options().software_cursor = !state.options().software_cursor;
            context_.cursor->software_mode(state.options().software_cursor);
            mark_dirty();
          }))
      .help_text("Draw the cursor with the game so it stays in sync with the "
                 "game.");

  if (!state.options().show_advanced_options) {
    return;
  }
  auto& options = state.options();
  auto changed = [this](int) { mark_dirty(); };
  add_menu_item(std::make_unique<IntSliderMenuItem>(
                    context_, "Key repeat delay", options.key_repeat_delay_ms,
                    GameOptions::KEY_REPEAT_DELAY_MIN,
                    GameOptions::KEY_REPEAT_DELAY_MAX, changed, face, 50))
      .advanced(true)
      .help_text("Milliseconds before a key starts repeating when held down.");
  add_menu_item(std::make_unique<IntSliderMenuItem>(
                    context_, "Key repeat interval",
                    options.key_repeat_interval_ms,
                    GameOptions::KEY_REPEAT_INTERVAL_MIN,
                    GameOptions::KEY_REPEAT_INTERVAL_MAX, changed, face, 10))
      .advanced(true)
      .help_text("Milliseconds between repeats while a key is held down.");
  add_menu_item(std::make_unique<IntSliderMenuItem>(
                    context_, "Tooltip delay", options.tooltip_delay_ms,
                    GameOptions::TOOLTIP_DELAY_MIN,
                    GameOptions::TOOLTIP_DELAY_MAX, changed, face, 100))
      .advanced(true)
      .help_text("Milliseconds the pointer rests on something before its "
                 "tooltip shows.");
}

MenuItem* MenuOptions::add_save_and_back(GameState& state,
                                         OptionsScreen screen) {
  // Leaves room below for the help text.
  constexpr int scroll_height = 170;
  end_scrolling_items(scroll_height);

  MenuItem* advanced_toggle = nullptr;
  if (screen == OptionsScreen::Controls || screen == OptionsScreen::Gameplay ||
      screen == OptionsScreen::Rules) {
    // The bottom left corner, level with Back
    constexpr int toggle_y = 238;
    add_menu_item(
        std::make_unique<PositionMarkerMenuItem>(context_, 0, toggle_y));
    advanced_toggle = &add_menu_item(std::make_unique<CheckboxMenuItem>(
        context_, "Show advanced options",
        state.options().show_advanced_options, [&state, screen] {
          bool& show = state.options().show_advanced_options;
          show = !show;
          state.ui().replace_top<OptionsMenuState>(OptionsMenuState{
              .screen = screen, .focus_advanced_toggle = true});
        }));
  }

  // Back is the only way out, so it also saves. Outside a running game the
  // game rules are the ones new games start with.
  add_back([this, &state] {
    if (!state.ui().contains<CityViewState>()) {
      state.rules() = state.options().rules;
    }
    context_.config->save_options(state.options());
    state.ui().pop();
  });
  return advanced_toggle;
}

Surface MenuOptions::background() const { return default_background(title_); }
