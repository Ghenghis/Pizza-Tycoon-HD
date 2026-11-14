/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gfx {
struct HdImage;
}

namespace resources {

// A TrueType font that stands in for one of the game's 1994 bitmap fonts
// when the game draws at HD resolution (see Font::render_line_onto). Text is
// rasterized with anti-aliasing at the exact texel size it is shown at, so it
// is sharp at any hd_scale().
class HdTypeface {
public:
  struct Settings {
    std::string file;       // .ttf in the fonts folder
    float size = 1.0f;      // cap height, as a multiple of the original's
    float tracking = 0.0f;  // extra space between letters, original pixels
    bool uppercase = false; // the original has capitals only
  };

  // A line of text as 8-bit coverage. Column 0 is `left` texels from the
  // pen's start (negative when the first letter overhangs to the left);
  // row `baseline` is the baseline.
  // One letter of a line as it is placed in Coverage::pixels: its slot in
  // the glyph sheet (see Sheet) and its top-left texel there.
  struct PlacedGlyph {
    int slot = 0;
    int x = 0;
    int y = 0;
  };
  struct Coverage {
    int width = 0;
    int height = 0;
    int left = 0;
    int baseline = 0;
    std::vector<uint8_t> pixels;
    // The letters, when every one of them is in the glyph sheet of this
    // cap height (printable ASCII, line not narrowed); else empty.
    std::vector<PlacedGlyph> glyphs;
    bool from_sheet = false;
    float squeeze = 1.0f; // letters narrowed to this to fit (1: not)
    [[nodiscard]] bool empty() const noexcept { return width <= 0; }
  };

  // Every printable ASCII letter at one cap height, at both pen phases, in
  // one picture: white texels whose alpha is the letter's coverage. Text is
  // then drawn as one quad per letter from it, coloured by the GPU, so a new
  // line of text needs no new picture and no upload.
  struct Slot {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
  struct Sheet {
    std::shared_ptr<const gfx::HdImage> image;
    std::vector<Slot> slots; // by slot number (see PlacedGlyph)
  };

  // Null when the file can't be read or isn't a usable TrueType font.
  [[nodiscard]] static std::unique_ptr<HdTypeface>
  load(std::vector<uint8_t> ttf, Settings settings);
  [[nodiscard]] static std::unique_ptr<HdTypeface>
  load(const std::filesystem::path& file, Settings settings);

  ~HdTypeface();
  HdTypeface(const HdTypeface&) = delete;
  HdTypeface& operator=(const HdTypeface&) = delete;

  [[nodiscard]] const Settings& settings() const noexcept { return settings_; }
  [[nodiscard]] bool has_glyph(char32_t c) const;

  // Lays out text with the font's own advances and kerning plus the
  // tracking, cap height `cap_px` texels, `tracking_px` texels between
  // letters. A line wider than max_width texels is narrowed to fit.
  [[nodiscard]] Coverage line(std::u32string_view text, float cap_px,
                              float tracking_px, float max_width) const;

  // Draws the printable ASCII letters at this cap height ahead of time, on
  // a background thread, so the first screens full of text don't wait for
  // them.
  void prewarm(float cap_px) const;
  // Blocks until every prewarm started so far has finished.
  void wait_for_prewarm() const;
  // The glyph sheet of this cap height, once a prewarm has made it; null
  // before.
  [[nodiscard]] std::shared_ptr<const Sheet> sheet(float cap_px) const;
  // Sheet pictures made since the last call (to upload ahead of use).
  void
  take_new_sheets(std::vector<std::shared_ptr<const gfx::HdImage>>& out) const;

  // Pen advance of the whole line before narrowing, in texels.
  [[nodiscard]] float measure(std::u32string_view text, float cap_px,
                              float tracking_px) const;

private:
  HdTypeface() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Settings settings_;
};

// The HD typefaces for the game's fonts, read once from
// <data dir>/fonts/hd_fonts.json (font name -> settings).
class HdTypefaces {
public:
  static HdTypefaces& instance();
  // Loads the table from this folder; an empty path or a missing file turns
  // HD text off. Safe to call again (tests).
  void configure(const std::filesystem::path& folder);
  // Null when there is no HD typeface for this font name.
  [[nodiscard]] std::shared_ptr<const HdTypeface>
  get(std::string_view font_name) const;
  // Blocks until the letters drawn ahead of time are ready in every
  // typeface (benchmarks measure the state after the intro).
  void wait_for_prewarm() const;
  // Glyph sheet pictures made since the last call, of every typeface.
  [[nodiscard]] std::vector<std::shared_ptr<const gfx::HdImage>>
  take_new_sheets() const;

  // The hd_text option: off draws the original fonts even when HD
  // typefaces are loaded.
  static void set_enabled(bool on) noexcept;
  [[nodiscard]] static bool enabled() noexcept;
  // Text drawn from glyph sheets (on) or as one picture per line (off: the
  // fallback, and for comparing the two in tests).
  static void set_sheets_enabled(bool on) noexcept;
  [[nodiscard]] static bool sheets_enabled() noexcept;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<const HdTypeface>> fonts_;
};

} // namespace resources
