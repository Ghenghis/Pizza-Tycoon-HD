/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/util/ConfigManager.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include "core/food/PizzaJudging.h"
#include "core/state/GameOptions.h"
#include "core/state/GameRules.h"
#include "core/util/AtomicFile.h"
#include "engine/Logger.h"
#include "engine/StringUtil.h"
#include "engine/gfx/HdImage.h"
#include "resources/util/FileUtils.h"

ConfigManager::ConfigManager(std::string filename)
    : filename_(std::move(filename)) {}

namespace { // anonymous

std::string from_bool(bool value) { return value ? "true" : "false"; }

bool to_bool(const std::string& text) { return text == "true" || text == "1"; }

struct OptionDef {
  std::string_view key;
  std::string (*get)(const GameOptions&);
  void (*set)(GameOptions&, const std::string&);
  std::string_view section;
  std::string_view note;
};

template <auto Field>
OptionDef bool_option(std::string_view key, std::string_view section = {},
                      std::string_view note = {}) {
  return {key, [](const GameOptions& o) { return from_bool(o.*Field); },
          [](GameOptions& o, const std::string& s) { o.*Field = to_bool(s); },
          section, note};
}

template <auto Field>
OptionDef float_option(std::string_view key, std::string_view section = {},
                       std::string_view note = {}) {
  return {key, [](const GameOptions& o) { return std::to_string(o.*Field); },
          [](GameOptions& o, const std::string& s) {
            o.*Field = std::strtof(s.c_str(), nullptr);
          },
          section, note};
}

template <auto Field, int Lowest, int Highest>
OptionDef int_option(std::string_view key, std::string_view section = {},
                     std::string_view note = {}) {
  return {key, [](const GameOptions& o) { return std::to_string(o.*Field); },
          [](GameOptions& o, const std::string& s) {
            int parsed = 0;
            auto [ptr, ec] =
                std::from_chars(s.data(), s.data() + s.size(), parsed);
            if (ec == std::errc{}) {
              o.*Field = std::clamp(parsed, Lowest, Highest);
            }
          },
          section, note};
}

template <auto Field>
OptionDef string_option(std::string_view key, std::string_view section = {},
                        std::string_view note = {}) {
  return {key, [](const GameOptions& o) { return std::string(o.*Field); },
          [](GameOptions& o, const std::string& s) { o.*Field = s; }, section,
          note};
}

template <auto Field>
OptionDef rule_bool_option(std::string_view key, std::string_view section = {},
                           std::string_view note = {}) {
  return {
      key, [](const GameOptions& o) { return from_bool(o.rules.*Field); },
      [](GameOptions& o, const std::string& s) { o.rules.*Field = to_bool(s); },
      section, note};
}

template <auto Field, auto Parser>
OptionDef rule_enum_option(std::string_view key, std::string_view section = {},
                           std::string_view note = {}) {
  return {key,
          [](const GameOptions& o) {
            return std::string(to_config_value(o.rules.*Field));
          },
          [](GameOptions& o, const std::string& s) {
            if (auto parsed = Parser(s)) {
              o.rules.*Field = *parsed;
            }
          },
          section, note};
}

// Parser is the matching *_from_config_value function. Returns nullopt for a
// value it doesn't recognise.
template <auto Field, auto Parser>
OptionDef enum_option(std::string_view key, std::string_view section = {},
                      std::string_view note = {}) {
  return {key,
          [](const GameOptions& o) {
            return std::string(to_config_value(o.*Field));
          },
          [](GameOptions& o, const std::string& s) {
            if (auto parsed = Parser(s)) {
              o.*Field = *parsed;
            }
          },
          section, note};
}

// The picture settings (GameOptions::picture, engine/fx/Look.h).
template <int fx::Sliders::*Field>
OptionDef picture_slider_option(std::string_view key,
                                std::string_view section = {},
                                std::string_view note = {}) {
  return {key,
          [](const GameOptions& o) {
            return std::to_string(o.picture.sliders.*Field);
          },
          [](GameOptions& o, const std::string& s) {
            int parsed = 0;
            auto [ptr, ec] =
                std::from_chars(s.data(), s.data() + s.size(), parsed);
            if (ec == std::errc{}) {
              o.picture.sliders.*Field =
                  std::clamp(parsed, fx::Sliders::MIN, fx::Sliders::MAX);
            }
          },
          section, note};
}

template <fx::Area Where>
OptionDef picture_area_option(std::string_view key,
                              std::string_view section = {},
                              std::string_view note = {}) {
  return {key,
          [](const GameOptions& o) {
            const auto it = o.picture.per_area.find(Where);
            return std::string(fx::to_config_value(
                it == o.picture.per_area.end() ? fx::AreaPreset::Same
                                               : it->second));
          },
          [](GameOptions& o, const std::string& s) {
            if (auto parsed = fx::area_preset_from_config_value(s)) {
              if (*parsed == fx::AreaPreset::Same) {
                o.picture.per_area.erase(Where);
              }
              else {
                o.picture.per_area[Where] = *parsed;
              }
            }
          },
          section, note};
}

template <fx::Area Where>
OptionDef weather_option(std::string_view key, std::string_view section = {},
                         std::string_view note = {}) {
  return {key,
          [](const GameOptions& o) {
            return std::string(
                fx::to_config_value(o.picture.weather_for(Where)));
          },
          [](GameOptions& o, const std::string& s) {
            if (auto parsed = fx::weather_from_config_value(s)) {
              if (*parsed == fx::Weather::None) {
                o.picture.weather.erase(Where);
              }
              else {
                o.picture.weather[Where] = *parsed;
              }
            }
          },
          section, note};
}

const auto option_defs = std::to_array({
    bool_option<&GameOptions::crt_aspect_ratio>(
        "crt_aspect_ratio", "Display", "Use the original 4:3 CRT aspect ratio"),
    bool_option<&GameOptions::double_size>("double_size", {},
                                           "Render at 2x scale"),
    bool_option<&GameOptions::fullscreen>("fullscreen"),
    enum_option<&GameOptions::render_fps, &fps_option_from_config_value>(
        "render_fps", {}, "30, 60 or 120"),
    bool_option<&GameOptions::vsync>("vsync"),
    bool_option<&GameOptions::smooth_motion>(
        "smooth_motion", {},
        "Walking characters glide between steps instead of jumping"),
    bool_option<&GameOptions::lighting>(
        "lighting", {},
        "HD: candles, ovens and screens light the room around them"),
    bool_option<&GameOptions::hd_text>(
        "hd_text", {},
        "HD: sharp modern typefaces instead of the 1994 bitmap fonts"),
    bool_option<&GameOptions::software_cursor>("software_cursor"),
    enum_option<&GameOptions::smoothing, &smoothing_from_config_value>(
        "smoothing", {},
        "off, text or all; round off the blocky edges of the original art"),
    int_option<&GameOptions::hd_scale, 0, gfx::max_hd_scale>(
        "hd_scale", {},
        "0 = Auto (from the screen's height); 1 = original 640x400; 2-8 "
        "renders that many texels per pixel (3 = 1920x1200, 6 = 3840x2400). "
        "Needs a restart"),
    string_option<&GameOptions::hd_pack>(
        "hd_pack", {},
        "folder of HD replacement art; empty uses the hd folder next to the "
        "program if there is one. Needs a restart"),

    bool_option<&GameOptions::map_fx>("map_fx", "Sound"),
    bool_option<&GameOptions::restaurant_fx>("restaurant_fx"),
    bool_option<&GameOptions::music>("music"),
    float_option<&GameOptions::sound_volume>("sound_volume", {}, "0.0 to 1.0"),
    float_option<&GameOptions::music_volume>("music_volume", {}, "0.0 to 1.0"),
    float_option<&GameOptions::master_volume>("master_volume", {},
                                              "0.0 to 1.0, music and effects"),
    bool_option<&GameOptions::mute_unfocused>("mute_unfocused"),
    bool_option<&GameOptions::enhanced_effects>("enhanced_effects"),
    bool_option<&GameOptions::duck_music>("duck_music"),

    bool_option<&GameOptions::rmb_drag_inverted>("rmb_drag_inverted", "Input"),
    bool_option<&GameOptions::scroll_wheel_inverted>("scroll_wheel_inverted"),

    enum_option<&GameOptions::autosave_frequency,
                &autosave_frequency_from_config_value>(
        "autosave_frequency", "Saving", "off, daily, weekly or monthly"),
    int_option<&GameOptions::autosave_count, GameOptions::AUTOSAVE_COUNT_MIN,
               GameOptions::AUTOSAVE_COUNT_MAX>("autosave_count", {},
                                                "1 to 9 rotating slots"),
    bool_option<&GameOptions::load_list_sort_by_date>("load_list_sort_by_date"),
    bool_option<&GameOptions::load_list_show_autosaves>(
        "load_list_show_autosaves"),

    bool_option<&GameOptions::automatic_report>(
        "automatic_report", "Gameplay",
        "show the day's turnover report when the turn ends"),
    rule_bool_option<&GameRules::fix_legacy_bugs>("fix_legacy_bugs"),
    bool_option<&GameOptions::qol_improvements>("qol_improvements"),
    rule_bool_option<&GameRules::cook_book>(
        "cook_book", {},
        "allow a standard pizza's cook book entry to be shown in the recipe "
        "list"),
    bool_option<&GameOptions::show_player_color_in_dropdown>(
        "show_player_color_in_dropdown", {},
        "mark players in the city map dropdowns with their player color"),

    rule_bool_option<&GameRules::mafia_shakedowns_target_all_restaurants>(
        "mafia_shakedowns_target_all_restaurants", "Advanced",
        "shake down a random restaurant instead of always the first"),
    rule_bool_option<&GameRules::demand_fulfillment_popularity>(
        "demand_fulfillment_popularity", {},
        "fix bug in order fulfillment popularity"),
    rule_bool_option<&GameRules::suppliers_remember_missed_payments>(
        "suppliers_remember_missed_payments", {},
        "a supplier holds a missed payment against you when negotiating"),
    rule_enum_option<&GameRules::win_condition,
                     &win_condition_from_config_value>(
        "win_condition", {}, "paris, any_city or all_cities"),
    rule_enum_option<&GameRules::pizza_judging,
                     &pizza_judging_from_config_value>(
        "pizza_judging", {},
        "original judges a pizza on a standard recipe very strictly, "
        "relaxed does not care about the exact form and positioning"),
    int_option<&GameOptions::attack_scene_hz, GameOptions::ATTACK_SCENE_HZ_MIN,
               GameOptions::ATTACK_SCENE_HZ_MAX>(
        "attack_scene_hz", {},
        "attack scene updates per second, 70 for a fast 486 or 35 for a slower "
        "machine"),
    int_option<&GameOptions::key_repeat_delay_ms,
               GameOptions::KEY_REPEAT_DELAY_MIN,
               GameOptions::KEY_REPEAT_DELAY_MAX>(
        "key_repeat_delay_ms", {},
        "milliseconds before a key you're holding down starts repeating"),
    int_option<&GameOptions::key_repeat_interval_ms,
               GameOptions::KEY_REPEAT_INTERVAL_MIN,
               GameOptions::KEY_REPEAT_INTERVAL_MAX>(
        "key_repeat_interval_ms", {}, "milliseconds between repeats"),
    int_option<&GameOptions::tooltip_delay_ms, GameOptions::TOOLTIP_DELAY_MIN,
               GameOptions::TOOLTIP_DELAY_MAX>(
        "tooltip_delay_ms", {},
        "milliseconds the pointer rests on an item before its tooltip shows"),

    OptionDef{"picture_preset",
              [](const GameOptions& o) {
                return std::string(fx::to_config_value(o.picture.preset));
              },
              [](GameOptions& o, const std::string& s) {
                if (auto parsed = fx::preset_from_config_value(s)) {
                  o.picture.preset = *parsed;
                }
              },
              "Picture",
              "HD: classic, natural, vivid, cinematic or studio; classic is "
              "the HD look as it always was"},
    picture_slider_option<&fx::Sliders::intensity>(
        "picture_intensity", {},
        "-50 to 50, 0 = as the preset has it: brightness and lights"),
    picture_slider_option<&fx::Sliders::shadows>(
        "picture_shadows", {}, "-50 to 50: how deep the shadows go"),
    picture_slider_option<&fx::Sliders::bloom>(
        "picture_bloom", {},
        "-50 to 50: glow around bright things (GPU renderer)"),
    picture_slider_option<&fx::Sliders::warmth>(
        "picture_warmth", {}, "-50 to 50: + warmer, - cooler"),
    picture_slider_option<&fx::Sliders::vignette>(
        "picture_vignette", {}, "-50 to 50: darker corners (GPU renderer)"),
    OptionDef{
        "picture_crt",
        [](const GameOptions& o) { return from_bool(o.picture.sliders.crt); },
        [](GameOptions& o, const std::string& s) {
          o.picture.sliders.crt = to_bool(s);
        },
        {},
        "scanlines and a soft aperture mask (GPU renderer)"},
    picture_area_option<fx::Area::City>(
        "picture_city", {}, "same, or a preset of its own for the city map"),
    picture_area_option<fx::Area::Restaurant>("picture_restaurant"),
    picture_area_option<fx::Area::Kitchen>("picture_kitchen"),
    picture_area_option<fx::Area::NorthPole>("picture_north_pole"),
    enum_option<&GameOptions::renderer, &fx::renderer_from_config_value>(
        "renderer", {},
        "auto (SDL's choice) or gpu (SDL's GPU driver, needed for bloom, "
        "vignette, CRT and the full colour grade). Needs a restart"),
    bool_option<&GameOptions::hdr_output>(
        "hdr_output", {},
        "HDR (scRGB) output when the display has HDR on. Needs a restart"),
    OptionDef{
        "picture_day_night",
        [](const GameOptions& o) { return from_bool(o.picture.day_night); },
        [](GameOptions& o, const std::string& s) {
          o.picture.day_night = to_bool(s);
        },
        {},
        "the game clock tints the city map and lights the lamps at "
        "night; emissive art glows (not in the classic preset)"},
    OptionDef{"picture_reduced_motion",
              [](const GameOptions& o) {
                return from_bool(o.picture.reduced_motion);
              },
              [](GameOptions& o, const std::string& s) {
                o.picture.reduced_motion = to_bool(s);
              },
              {},
              "fewer, slower snowflakes with no sway"},
    weather_option<fx::Area::City>("weather_city", {},
                                   "none, snow or light_snow"),
    weather_option<fx::Area::Restaurant>("weather_restaurant"),
    weather_option<fx::Area::Kitchen>("weather_kitchen"),
    weather_option<fx::Area::NorthPole>("weather_north_pole"),
    OptionDef{"picture_screenshots",
              [](const GameOptions& o) {
                return from_bool(o.picture.graded_screenshots);
              },
              [](GameOptions& o, const std::string& s) {
                o.picture.graded_screenshots = to_bool(s);
              },
              {},
              "screenshots show the picture as the window does (preset, "
              "weather); off writes the frame underneath"},
});

// Where the original game lives.
constexpr std::string_view game_path_key = "game_path";

// Settings the game stores that are not options.
constexpr std::string_view non_option_keys[] = {game_path_key};

bool known_key(std::string_view key) {
  const auto matches = [key](std::string_view known) { return known == key; };
  return std::ranges::any_of(non_option_keys, matches) ||
         std::ranges::any_of(option_defs, matches, &OptionDef::key);
}

constexpr std::string_view settings_preamble =
    R"(# Pizza Legacy settings
#
# Each uncommented line sets one option:
#
#   option=value
#
# Only options you change need a line; anything absent uses the defaults shown
# below. If this file is missing it will be regenerated with the defaults on
# relaunch.
#
# Defaults:
)";

} // namespace

bool ConfigManager::load_config() {
  if (!fileutils::fexists(filename_)) {
    return false;
  }

  std::ifstream file(filename_);
  if (!file.is_open()) {
    return false;
  }

  std::string line;
  while (std::getline(file, line)) {
    const std::string_view stripped = util::trim(line);
    if (stripped.empty() || stripped.starts_with('#')) {
      continue;
    }
    const auto delimiterPos = stripped.find('=');
    if (delimiterPos == std::string_view::npos) {
      continue;
    }
    std::string key(util::trim(stripped.substr(0, delimiterPos)));
    if (!known_key(key)) {
      Logger::warn("{}: dropping unknown setting '{}'", filename_, key);
      continue;
    }
    std::string value(util::trim(stripped.substr(delimiterPos + 1)));
    config_[std::move(key)] = std::move(value);
  }
  return true;
}

bool ConfigManager::save_config() {
  // Built in memory and written in one step, so a failed write leaves the old
  // settings file as it was.
  std::ostringstream file;
  file << settings_preamble;

  const GameOptions defaults;
  for (const auto& def : option_defs) {
    if (!def.section.empty()) {
      file << "#\n# " << def.section << ":\n";
    }
    if (!def.note.empty()) {
      file << "# " << def.note << "\n";
    }
    file << '#' << def.key << '=' << def.get(defaults) << '\n';
  }
  file << "\n";

  for (const auto& [key, value] : config_) {
    file << key << "=" << value << "\n";
  }

  const util::AtomicWriteResult result =
      util::write_file_atomically(filename_, file.str());
  if (!result) {
    Logger::warn("Settings not saved: {}", result.error);
    return false;
  }
  return true;
}

const std::string ConfigManager::value(const std::string& key) const {
  static const std::string empty{};
  auto it = config_.find(key);
  if (it != config_.end()) {
    return it->second;
  }
  return empty;
}

void ConfigManager::value(const std::string& key, const std::string& value) {
  config_[key] = value;
}

bool ConfigManager::save_options(const GameOptions& opts) {
  const GameOptions defaults;
  for (const auto& def : option_defs) {
    std::string value = def.get(opts);
    if (value == def.get(defaults)) {
      config_.erase(std::string(def.key));
    }
    else {
      config_[std::string(def.key)] = std::move(value);
    }
  }

  return save_config();
}

GameOptions ConfigManager::load_options() const {
  GameOptions opts;

  for (const auto& def : option_defs) {
    auto it = config_.find(std::string(def.key));
    if (it != config_.end()) {
      def.set(opts, it->second);
    }
  }

  return opts;
}

std::string ConfigManager::active_game_path() const {
  return value(std::string(game_path_key));
}

void ConfigManager::active_game_path(const std::string& path) {
  value(std::string(game_path_key), path);
}
