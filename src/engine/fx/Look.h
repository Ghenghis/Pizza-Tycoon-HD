/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// The picture settings of the HD remaster, like a TV's picture modes: a
// preset, a few sliders on top of it, and a preset per area of the game.
// Only the picture changes; nothing here reaches the game simulation.
//
// The settings resolve to a Grade, the numbers the final picture pass works
// with (fx::PostFx on the GPU, fx::grade_frame on the CPU for the tests).
// The Classic preset with the sliders at 0 is the identity: the frame is
// drawn exactly as it was before any of this existed.
namespace fx {

enum class Preset : uint8_t {
  Classic,   // the HD look exactly as it always was
  Natural,   // a touch of contrast, warmth and glow
  Vivid,     // rich colour, bright lights
  Cinematic, // warm highlights, deep shadows, glow and a vignette
  Studio,    // neutral and accurate, lifted shadows
};
inline constexpr std::array all_presets{Preset::Classic, Preset::Natural,
                                        Preset::Vivid, Preset::Cinematic,
                                        Preset::Studio};

// A per-area choice: the main preset, or one of its own.
enum class AreaPreset : uint8_t {
  Same,
  Classic,
  Natural,
  Vivid,
  Cinematic,
  Studio,
};
inline constexpr std::array all_area_presets{
    AreaPreset::Same,  AreaPreset::Classic,   AreaPreset::Natural,
    AreaPreset::Vivid, AreaPreset::Cinematic, AreaPreset::Studio};

// Where in the game the frame is. Other is everything else (menus, reports,
// the newspaper), which follows the main preset.
enum class Area : uint8_t { Other, City, Restaurant, Kitchen, NorthPole };
inline constexpr std::array overridable_areas{Area::City, Area::Restaurant,
                                              Area::Kitchen, Area::NorthPole};

// Which SDL render driver the game asks for. Auto is SDL's choice
// (Direct3D 11 on Windows); Gpu is SDL's GPU driver (Direct3D 12, Vulkan or
// Metal), the only one that runs the post-FX shader.
enum class RendererChoice : uint8_t { Auto, Gpu };

// Labels for the options screen.
[[nodiscard]] std::string to_string(Preset p);
[[nodiscard]] std::string to_string(AreaPreset p);
[[nodiscard]] std::string to_string(RendererChoice c);
[[nodiscard]] std::string to_string(Area a);

// settings.conf values: classic, natural, vivid, cinematic, studio; same;
// auto, gpu.
[[nodiscard]] std::string_view to_config_value(Preset p);
[[nodiscard]] std::string_view to_config_value(AreaPreset p);
[[nodiscard]] std::string_view to_config_value(RendererChoice c);
[[nodiscard]] std::optional<Preset> preset_from_config_value(std::string_view);
[[nodiscard]] std::optional<AreaPreset>
    area_preset_from_config_value(std::string_view);
[[nodiscard]] std::optional<RendererChoice>
    renderer_from_config_value(std::string_view);

// The sliders, each -50..50 with 0 meaning "as the preset has it".
struct Sliders {
  static constexpr int MIN = -50;
  static constexpr int MAX = 50;
  int intensity = 0; // overall brightness and the strength of the lights
  int shadows = 0;   // how deep the ambient darkness and the blacks go
  int bloom = 0;     // glow around bright things
  int warmth = 0;    // white balance: + warmer (amber), - cooler (blue)
  int vignette = 0;  // darkening towards the corners
  bool crt = false;  // scanlines and a soft aperture mask
  bool operator==(const Sliders&) const = default;
};

struct LookSettings {
  Preset preset = Preset::Classic;
  Sliders sliders;
  // Areas with their own preset; absent or Same follows `preset`.
  std::map<Area, AreaPreset> per_area;

  [[nodiscard]] Preset preset_for(Area area) const;
  bool operator==(const LookSettings&) const = default;
};

// The numbers of the picture pass, in the order it applies them to a texel
// (sRGB-encoded values, 0..1):
//   bloom: c += glow * over^2 * bloom   (over: the glow above the threshold)
//   exposure and white balance: c *= exposure * balance()
//   contrast around mid grey: c = (c - 0.5) * contrast + 0.5
//   saturation around the Rec. 709 luma
//   lift / gamma / gain: c = gain * (c + lift * (1 - c)); c = c^(1/gamma)
//   vignette, then the CRT rows and mask; clamped to 0..1.
// light_strength and shadow_depth go to the light pass instead (the room's
// light pools and its ambient darkness, LightPass::Strength).
struct Grade {
  float exposure = 1.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float warmth = 0.0f; // -1 (blue) .. 1 (amber)
  std::array<float, 3> lift{0.0f, 0.0f, 0.0f};
  std::array<float, 3> gamma{1.0f, 1.0f, 1.0f};
  std::array<float, 3> gain{1.0f, 1.0f, 1.0f};
  float vignette = 0.0f; // 0..1
  float bloom = 0.0f;    // 0..1
  float bloom_threshold = 0.7f;
  float crt = 0.0f; // 0..1
  float light_strength = 1.0f;
  float shadow_depth = 1.0f;

  // True when the pass would not change a single texel.
  [[nodiscard]] bool identity() const;
  // True when the picture pass itself has nothing to do (the light pass may
  // still have: see lights_identity()).
  [[nodiscard]] bool picture_identity() const;
  [[nodiscard]] bool lights_identity() const;
  // Only exposure and white balance, the part fixed-function SDL can do.
  [[nodiscard]] bool modulation_only() const;
  // White balance multipliers for `warmth`, at constant luma.
  [[nodiscard]] std::array<float, 3> balance() const;
  // exposure * balance(), per channel.
  [[nodiscard]] std::array<float, 3> modulation() const;
  bool operator==(const Grade&) const = default;
};

[[nodiscard]] Grade preset_grade(Preset p);
// The preset of `area` with the sliders applied.
[[nodiscard]] Grade resolve(const LookSettings& settings, Area area);

// The look of the frame being drawn, set by the game loop once per frame
// (like lighting::set_enabled) and read by the renderer. Starts as the
// identity.
void set_frame_look(const LookSettings& settings, Area area);
void set_frame_grade(const Grade& grade);
[[nodiscard]] const Grade& frame_grade();

} // namespace fx
