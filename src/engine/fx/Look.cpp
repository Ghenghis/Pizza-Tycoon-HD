/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/Look.h"

#include <algorithm>
#include <charconv>
#include "engine/fx/DayNight.h"

namespace fx {

std::string to_string(Preset p) {
  switch (p) {
  case Preset::Classic:
    return "Classic";
  case Preset::Natural:
    return "Natural";
  case Preset::Vivid:
    return "Vivid";
  case Preset::Cinematic:
    return "Cinematic";
  case Preset::Studio:
    return "Studio";
  }
  return "Classic";
}

std::string to_string(AreaPreset p) {
  switch (p) {
  case AreaPreset::Same:
    return "Same as Preset";
  case AreaPreset::Classic:
    return to_string(Preset::Classic);
  case AreaPreset::Natural:
    return to_string(Preset::Natural);
  case AreaPreset::Vivid:
    return to_string(Preset::Vivid);
  case AreaPreset::Cinematic:
    return to_string(Preset::Cinematic);
  case AreaPreset::Studio:
    return to_string(Preset::Studio);
  }
  return "Same as Preset";
}

std::string to_string(RendererChoice c) {
  switch (c) {
  case RendererChoice::Auto:
    return "Auto";
  case RendererChoice::Gpu:
    return "GPU (SDL GPU)";
  }
  return "Auto";
}

std::string to_string(Area a) {
  switch (a) {
  case Area::Other:
    return "Menus";
  case Area::City:
    return "City map";
  case Area::Restaurant:
    return "Restaurant";
  case Area::Kitchen:
    return "Kitchen";
  case Area::NorthPole:
    return "North Pole";
  }
  return "Menus";
}

std::string to_string(Weather w) {
  switch (w) {
  case Weather::None:
    return "None";
  case Weather::Snow:
    return "Snow";
  case Weather::LightSnow:
    return "Light snow";
  }
  return "None";
}

std::string to_string(AmbientMotion m) {
  switch (m) {
  case AmbientMotion::Off:
    return "Off";
  case AmbientMotion::Static:
    return "Static";
  case AmbientMotion::Reduced:
    return "Reduced";
  case AmbientMotion::Full:
    return "Full";
  }
  return "Full";
}

std::string_view to_config_value(AmbientMotion m) {
  switch (m) {
  case AmbientMotion::Off:
    return "off";
  case AmbientMotion::Static:
    return "static";
  case AmbientMotion::Reduced:
    return "reduced";
  case AmbientMotion::Full:
    return "full";
  }
  return "full";
}

std::optional<AmbientMotion>
ambient_motion_from_config_value(std::string_view v) {
  for (AmbientMotion m : all_ambient_motion) {
    if (to_config_value(m) == v) {
      return m;
    }
  }
  return std::nullopt;
}

AmbientMotion LookSettings::effective_motion() const {
  if (reduced_motion && ambient_motion == AmbientMotion::Full) {
    return AmbientMotion::Reduced;
  }
  return ambient_motion;
}

std::string_view to_config_value(Weather w) {
  switch (w) {
  case Weather::None:
    return "none";
  case Weather::Snow:
    return "snow";
  case Weather::LightSnow:
    return "light_snow";
  }
  return "none";
}

std::optional<Weather> weather_from_config_value(std::string_view v) {
  for (Weather w : all_weather) {
    if (to_config_value(w) == v) {
      return w;
    }
  }
  return std::nullopt;
}

std::string_view to_config_value(Preset p) {
  switch (p) {
  case Preset::Classic:
    return "classic";
  case Preset::Natural:
    return "natural";
  case Preset::Vivid:
    return "vivid";
  case Preset::Cinematic:
    return "cinematic";
  case Preset::Studio:
    return "studio";
  }
  return "classic";
}

std::string_view to_config_value(AreaPreset p) {
  switch (p) {
  case AreaPreset::Same:
    return "same";
  case AreaPreset::Classic:
    return to_config_value(Preset::Classic);
  case AreaPreset::Natural:
    return to_config_value(Preset::Natural);
  case AreaPreset::Vivid:
    return to_config_value(Preset::Vivid);
  case AreaPreset::Cinematic:
    return to_config_value(Preset::Cinematic);
  case AreaPreset::Studio:
    return to_config_value(Preset::Studio);
  }
  return "same";
}

std::string_view to_config_value(RendererChoice c) {
  return c == RendererChoice::Gpu ? "gpu" : "auto";
}

std::optional<Preset> preset_from_config_value(std::string_view value) {
  for (Preset p : all_presets) {
    if (to_config_value(p) == value) {
      return p;
    }
  }
  return std::nullopt;
}

std::optional<AreaPreset> area_preset_from_config_value(std::string_view v) {
  for (AreaPreset p : all_area_presets) {
    if (to_config_value(p) == v) {
      return p;
    }
  }
  return std::nullopt;
}

std::optional<RendererChoice> renderer_from_config_value(std::string_view v) {
  if (v == "auto") {
    return RendererChoice::Auto;
  }
  if (v == "gpu") {
    return RendererChoice::Gpu;
  }
  return std::nullopt;
}

Preset LookSettings::preset_for(Area area) const {
  const auto it = per_area.find(area);
  if (it == per_area.end()) {
    return preset;
  }
  switch (it->second) {
  case AreaPreset::Same:
    return preset;
  case AreaPreset::Classic:
    return Preset::Classic;
  case AreaPreset::Natural:
    return Preset::Natural;
  case AreaPreset::Vivid:
    return Preset::Vivid;
  case AreaPreset::Cinematic:
    return Preset::Cinematic;
  case AreaPreset::Studio:
    return Preset::Studio;
  }
  return preset;
}

Weather LookSettings::weather_for(Area area) const {
  const auto it = weather.find(area);
  return it == weather.end() ? Weather::None : it->second;
}

namespace {
bool all_equal(const std::array<float, 3>& v, float x) {
  return v[0] == x && v[1] == x && v[2] == x;
}
} // namespace

bool Grade::modulation_only() const {
  return contrast == 1.0f && saturation == 1.0f && all_equal(lift, 0.0f) &&
         all_equal(gamma, 1.0f) && all_equal(gain, 1.0f) && vignette <= 0.0f &&
         bloom <= 0.0f && crt <= 0.0f;
}

bool Grade::picture_identity() const {
  return modulation_only() && exposure == 1.0f && warmth == 0.0f;
}

bool Grade::lights_identity() const {
  return light_strength == 1.0f && shadow_depth == 1.0f;
}

bool Grade::identity() const { return picture_identity() && lights_identity(); }

std::array<float, 3> Grade::balance() const {
  if (warmth == 0.0f) {
    return {1.0f, 1.0f, 1.0f};
  }
  // Towards amber (more red, a little green, less blue) or towards blue,
  // scaled so the luma stays where it was.
  const float w = std::clamp(warmth, -1.0f, 1.0f);
  std::array<float, 3> m{1.0f + 0.10f * w, 1.0f + 0.015f * w, 1.0f - 0.14f * w};
  const float luma = 0.2126f * m[0] + 0.7152f * m[1] + 0.0722f * m[2];
  for (float& c : m) {
    c /= luma;
  }
  return m;
}

std::array<float, 3> Grade::modulation() const {
  std::array<float, 3> m = balance();
  for (float& c : m) {
    c *= exposure;
  }
  return m;
}

Grade preset_grade(Preset p) {
  Grade g;
  switch (p) {
  case Preset::Classic:
    break;
  case Preset::Natural:
    g.glow = 0.85f;
    g.contrast = 1.04f;
    g.saturation = 1.04f;
    g.warmth = 0.06f;
    g.vignette = 0.10f;
    g.bloom = 0.10f;
    g.bloom_threshold = 0.75f;
    break;
  case Preset::Vivid:
    g.glow = 1.0f;
    g.exposure = 1.04f;
    g.contrast = 1.10f;
    g.saturation = 1.25f;
    g.gain = {1.02f, 1.02f, 1.02f};
    g.vignette = 0.05f;
    g.bloom = 0.16f;
    g.bloom_threshold = 0.70f;
    g.light_strength = 1.15f;
    g.shadow_depth = 0.90f;
    break;
  case Preset::Cinematic:
    g.glow = 1.0f;
    g.exposure = 0.97f;
    g.contrast = 1.12f;
    g.saturation = 0.92f;
    g.warmth = 0.20f;
    // Cool shadows under warm highlights.
    g.lift = {-0.010f, 0.0f, 0.020f};
    g.gamma = {0.98f, 0.98f, 0.98f};
    g.gain = {1.03f, 1.0f, 0.97f};
    g.vignette = 0.30f;
    g.bloom = 0.24f;
    g.bloom_threshold = 0.62f;
    g.light_strength = 1.20f;
    g.shadow_depth = 1.20f;
    break;
  case Preset::Studio:
    g.glow = 0.7f;
    g.contrast = 1.02f;
    g.warmth = -0.04f;
    g.lift = {0.02f, 0.02f, 0.02f};
    g.shadow_depth = 0.85f;
    break;
  }
  return g;
}

Grade resolve(const LookSettings& settings, Area area) {
  Grade g = preset_grade(settings.preset_for(area));
  const Sliders& s = settings.sliders;
  auto unit = [](int v) {
    return static_cast<float>(std::clamp(v, Sliders::MIN, Sliders::MAX)) /
           static_cast<float>(Sliders::MAX);
  };
  if (s.intensity != 0) {
    const float f = 1.0f + 0.5f * unit(s.intensity);
    g.exposure *= f;
    g.light_strength *= f;
  }
  if (s.shadows != 0) {
    g.shadow_depth *= 1.0f + 0.5f * unit(s.shadows);
    for (float& l : g.lift) {
      l -= 0.06f * unit(s.shadows);
    }
  }
  if (s.bloom != 0) {
    g.bloom = std::clamp(g.bloom + 0.4f * unit(s.bloom), 0.0f, 1.0f);
  }
  if (s.warmth != 0) {
    g.warmth = std::clamp(g.warmth + 0.6f * unit(s.warmth), -1.0f, 1.0f);
  }
  if (s.vignette != 0) {
    g.vignette = std::clamp(g.vignette + 0.5f * unit(s.vignette), 0.0f, 1.0f);
  }
  if (s.crt) {
    g.crt = 1.0f;
  }
  return g;
}

FrameState resolve_frame(const LookSettings& settings,
                         const FrameContext& context) {
  FrameState f;
  f.area = context.area;
  f.grade = resolve(settings, context.area);
  f.weather = settings.weather_for(context.area);
  f.reduced_motion = settings.reduced_motion;
  f.graded_capture = settings.graded_screenshots;
  // Day and night: places with a clock, never in the Classic preset.
  const bool clocked = context.area == Area::City ||
                       context.area == Area::Restaurant ||
                       context.area == Area::Kitchen;
  const bool classic = settings.preset_for(context.area) == Preset::Classic;
  if (clocked && !classic && f.grade.glow > 0.0f) {
    f.emissive =
        std::clamp(day_night(context.hours).night * f.grade.glow, 0.0f, 1.0f);
  }
  // The North Pole's ambient effects: its own location only, never a
  // Classic city, never in the 1994 look (the renderer drops the state).
  const AmbientMotion motion = settings.effective_motion();
  if (context.location == "north-pole" && motion != AmbientMotion::Off) {
    const float sky_night = day_night(context.hours).night;
    f.map_rect = context.map_rect;
    if (context.area == Area::City && context.map_rect[2] > 0) {
      f.aurora = sky_night;
    }
    f.twinkle = motion == AmbientMotion::Full;
    const double speed = motion == AmbientMotion::Full      ? 1.0
                         : motion == AmbientMotion::Reduced ? 0.25
                                                            : 0.0;
    if (speed == 0.0) {
      f.anim_seconds = 0.0; // Static: always the same picture
    }
    else if (context.anim_seconds >= 0.0) {
      f.anim_seconds = context.anim_seconds * speed;
    }
    // Its own snowfall on the city map unless a weather was chosen.
    if (context.area == Area::City && !settings.weather.contains(Area::City)) {
      f.weather = Weather::LightSnow;
    }
    if (motion != AmbientMotion::Full) {
      f.reduced_motion = true;
    }
    // The music accent: the glow follows the music's level a little.
    if (settings.music_accent && motion == AmbientMotion::Full &&
        f.emissive > 0.0f) {
      f.emissive =
          std::clamp(f.emissive * (1.0f + 0.25f * music_level()), 0.0f, 1.0f);
    }
  }
  if (settings.day_night && clocked && !classic) {
    const DayNight dn = day_night(context.hours);
    f.night = dn.night;
    f.sun_hours = context.hours;
    // Only the outdoor map takes the sky's tint; rooms have their own light.
    if (context.area == Area::City) {
      if (dn.exposure != 1.0f) {
        f.grade.exposure *= dn.exposure;
      }
      if (dn.warmth != 0.0f) {
        f.grade.warmth = std::clamp(f.grade.warmth + dn.warmth, -1.0f, 1.0f);
      }
    }
  }
  return f;
}

namespace {

FrameState g_frame;
float g_music_level = 0.0f;

struct AreaKey {
  std::string_view preset_key;
  std::string_view weather_key;
  Area area;
};
constexpr std::array area_keys{
    AreaKey{"picture_city", "weather_city", Area::City},
    AreaKey{"picture_restaurant", "weather_restaurant", Area::Restaurant},
    AreaKey{"picture_kitchen", "weather_kitchen", Area::Kitchen},
    AreaKey{"picture_north_pole", "weather_north_pole", Area::NorthPole},
};

struct SliderKey {
  std::string_view key;
  int Sliders::*field;
};
constexpr std::array slider_keys{
    SliderKey{"picture_intensity", &Sliders::intensity},
    SliderKey{"picture_shadows", &Sliders::shadows},
    SliderKey{"picture_bloom", &Sliders::bloom},
    SliderKey{"picture_warmth", &Sliders::warmth},
    SliderKey{"picture_vignette", &Sliders::vignette},
};

struct SwitchKey {
  std::string_view key;
  bool (*get)(const LookSettings&);
  void (*set)(LookSettings&, bool);
};
const std::array switch_keys{
    SwitchKey{"picture_crt",
              [](const LookSettings& s) { return s.sliders.crt; },
              [](LookSettings& s, bool v) { s.sliders.crt = v; }},
    SwitchKey{"picture_day_night",
              [](const LookSettings& s) { return s.day_night; },
              [](LookSettings& s, bool v) { s.day_night = v; }},
    SwitchKey{"picture_reduced_motion",
              [](const LookSettings& s) { return s.reduced_motion; },
              [](LookSettings& s, bool v) { s.reduced_motion = v; }},
    SwitchKey{"picture_screenshots",
              [](const LookSettings& s) { return s.graded_screenshots; },
              [](LookSettings& s, bool v) { s.graded_screenshots = v; }},
    SwitchKey{"picture_music_accent",
              [](const LookSettings& s) { return s.music_accent; },
              [](LookSettings& s, bool v) { s.music_accent = v; }},
};

} // namespace

const std::vector<std::string_view>& setting_keys() {
  static const std::vector<std::string_view> keys = [] {
    std::vector<std::string_view> k{"picture_preset"};
    for (const SliderKey& s : slider_keys) {
      k.push_back(s.key);
    }
    k.push_back("picture_crt");
    for (const AreaKey& a : area_keys) {
      k.push_back(a.preset_key);
    }
    k.push_back("picture_day_night");
    k.push_back("picture_reduced_motion");
    for (const AreaKey& a : area_keys) {
      k.push_back(a.weather_key);
    }
    k.push_back("picture_screenshots");
    k.push_back("picture_ambient_motion");
    k.push_back("picture_music_accent");
    return k;
  }();
  return keys;
}

std::optional<std::string> apply_setting(LookSettings& settings,
                                         std::string_view key,
                                         std::string_view value) {
  const std::string v(value);
  if (key == "picture_ambient_motion") {
    if (auto m = ambient_motion_from_config_value(value)) {
      settings.ambient_motion = *m;
      return std::nullopt;
    }
    return "Expected off, static, reduced or full, got '" + v + "'";
  }
  if (key == "picture_preset") {
    if (auto p = preset_from_config_value(value)) {
      settings.preset = *p;
      return std::nullopt;
    }
    return "Expected classic, natural, vivid, cinematic or studio, got '" + v +
           "'";
  }
  for (const SliderKey& s : slider_keys) {
    if (key == s.key) {
      int parsed = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return "Expected a number, got '" + v + "'";
      }
      if (parsed < Sliders::MIN || parsed > Sliders::MAX) {
        return std::string(key) + " takes -50 to 50, got '" + v + "'";
      }
      settings.sliders.*s.field = parsed;
      return std::nullopt;
    }
  }
  for (const SwitchKey& s : switch_keys) {
    if (key == s.key) {
      if (value == "true" || value == "on" || value == "1") {
        s.set(settings, true);
      }
      else if (value == "false" || value == "off" || value == "0") {
        s.set(settings, false);
      }
      else {
        return "Expected on or off, got '" + v + "'";
      }
      return std::nullopt;
    }
  }
  for (const AreaKey& a : area_keys) {
    if (key == a.preset_key) {
      const auto p = area_preset_from_config_value(value);
      if (!p) {
        return "Expected same or a preset, got '" + v + "'";
      }
      if (*p == AreaPreset::Same) {
        settings.per_area.erase(a.area);
      }
      else {
        settings.per_area[a.area] = *p;
      }
      return std::nullopt;
    }
    if (key == a.weather_key) {
      const auto w = weather_from_config_value(value);
      if (!w) {
        return "Expected none, snow or light_snow, got '" + v + "'";
      }
      if (*w == Weather::None) {
        settings.weather.erase(a.area);
      }
      else {
        settings.weather[a.area] = *w;
      }
      return std::nullopt;
    }
  }
  return "Unknown picture setting '" + std::string(key) + "'";
}

std::string setting_value(const LookSettings& settings, std::string_view key) {
  if (key == "picture_ambient_motion") {
    return std::string(to_config_value(settings.ambient_motion));
  }
  if (key == "picture_preset") {
    return std::string(to_config_value(settings.preset));
  }
  for (const SliderKey& s : slider_keys) {
    if (key == s.key) {
      return std::to_string(settings.sliders.*s.field);
    }
  }
  for (const SwitchKey& s : switch_keys) {
    if (key == s.key) {
      return s.get(settings) ? "true" : "false";
    }
  }
  for (const AreaKey& a : area_keys) {
    if (key == a.preset_key) {
      const auto it = settings.per_area.find(a.area);
      return std::string(to_config_value(
          it == settings.per_area.end() ? AreaPreset::Same : it->second));
    }
    if (key == a.weather_key) {
      return std::string(to_config_value(settings.weather_for(a.area)));
    }
  }
  return {};
}

void set_frame_look(const LookSettings& settings, const FrameContext& context) {
  g_frame = resolve_frame(settings, context);
}

void set_frame_look(const LookSettings& settings, Area area) {
  set_frame_look(settings, FrameContext{.area = area});
}

void set_frame_grade(const Grade& grade) {
  g_frame = FrameState{};
  g_frame.grade = grade;
}

void set_frame_state(const FrameState& state) { g_frame = state; }

void set_music_level(float level) {
  g_music_level = std::clamp(level, 0.0f, 1.0f);
}

float music_level() { return g_music_level; }

const Grade& frame_grade() { return g_frame.grade; }

const FrameState& frame_state() { return g_frame; }

} // namespace fx
