/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/Look.h"

#include <algorithm>

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
    g.contrast = 1.04f;
    g.saturation = 1.04f;
    g.warmth = 0.06f;
    g.vignette = 0.10f;
    g.bloom = 0.10f;
    g.bloom_threshold = 0.75f;
    break;
  case Preset::Vivid:
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

namespace {
Grade g_frame;
} // namespace

void set_frame_look(const LookSettings& settings, Area area) {
  g_frame = resolve(settings, area);
}

void set_frame_grade(const Grade& grade) { g_frame = grade; }

const Grade& frame_grade() { return g_frame; }

} // namespace fx
