/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resources/HdTypeface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>
#include "engine/Logger.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb/stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace resources {

namespace {

// Glyph bitmaps are cached per size and quarter-texel pen position, so a
// line that is drawn again (most of them) costs a copy, not a rasterization.
// Half-texel pen positions: at HD sizes finer steps don't show.
constexpr int subpixel_steps = 2;
constexpr std::size_t max_cached_glyphs = 4096;

struct GlyphKey {
  char32_t c;
  uint32_t scale_x_bits;
  uint32_t scale_y_bits;
  int sub;
  bool operator==(const GlyphKey&) const = default;
};

struct GlyphKeyHash {
  std::size_t operator()(const GlyphKey& k) const noexcept {
    std::size_t h = k.c;
    h = h * 1000003u ^ k.scale_x_bits;
    h = h * 1000003u ^ k.scale_y_bits;
    return h * 31u + static_cast<std::size_t>(k.sub);
  }
};

struct GlyphBitmap {
  int x0 = 0; // offset from the pen, texels
  int y0 = 0; // offset from the baseline (negative is up), texels
  int w = 0;
  int h = 0;
  std::vector<uint8_t> pixels;
};

uint32_t float_bits(float f) {
  uint32_t u = 0;
  static_assert(sizeof u == sizeof f);
  std::memcpy(&u, &f, sizeof u);
  return u;
}

} // namespace

struct HdTypeface::Impl {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  float cap_units = 0; // cap height in font units
  int ascent = 0;      // font units
  int descent = 0;     // font units, negative
  mutable std::mutex mutex;
  mutable std::unordered_map<GlyphKey, GlyphBitmap, GlyphKeyHash> cache;

  [[nodiscard]] int index(char32_t c) const {
    return stbtt_FindGlyphIndex(&info, static_cast<int>(c));
  }

  const GlyphBitmap& glyph(int gi, char32_t c, float sx, float sy,
                           int sub) const {
    const GlyphKey key{c, float_bits(sx), float_bits(sy), sub};
    if (auto it = cache.find(key); it != cache.end()) {
      return it->second;
    }
    const float shift = static_cast<float>(sub) / subpixel_steps;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&info, gi, sx, sy, shift, 0.0f, &x0, &y0,
                                    &x1, &y1);
    GlyphBitmap g;
    g.x0 = x0;
    g.y0 = y0;
    g.w = std::max(0, x1 - x0);
    g.h = std::max(0, y1 - y0);
    g.pixels.assign(static_cast<std::size_t>(g.w) * g.h, 0);
    if (g.w > 0 && g.h > 0) {
      stbtt_MakeGlyphBitmapSubpixel(&info, g.pixels.data(), g.w, g.h, g.w, sx,
                                    sy, shift, 0.0f, gi);
    }
    return cache.emplace(key, std::move(g)).first->second;
  }
};

HdTypeface::~HdTypeface() = default;

std::unique_ptr<HdTypeface> HdTypeface::load(std::vector<uint8_t> ttf,
                                             Settings settings) {
  auto impl = std::make_unique<Impl>();
  impl->data = std::move(ttf);
  if (impl->data.empty()) {
    return nullptr;
  }
  const int offset = stbtt_GetFontOffsetForIndex(impl->data.data(), 0);
  if (offset < 0 || !stbtt_InitFont(&impl->info, impl->data.data(), offset)) {
    return nullptr;
  }
  // Cap height from the outline of 'H': not every font fills in the OS/2
  // table, and this is what the original's cap height is matched against.
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  if (!stbtt_GetCodepointBox(&impl->info, 'H', &x0, &y0, &x1, &y1) || y1 <= 0) {
    return nullptr;
  }
  impl->cap_units = static_cast<float>(y1);
  int gap = 0;
  stbtt_GetFontVMetrics(&impl->info, &impl->ascent, &impl->descent, &gap);
  auto out = std::unique_ptr<HdTypeface>(new HdTypeface());
  out->impl_ = std::move(impl);
  out->settings_ = std::move(settings);
  return out;
}

std::unique_ptr<HdTypeface> HdTypeface::load(const std::filesystem::path& file,
                                             Settings settings) {
  std::ifstream is(file, std::ios::binary);
  if (!is) {
    return nullptr;
  }
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{is},
                             std::istreambuf_iterator<char>{}};
  return load(std::move(bytes), std::move(settings));
}

bool HdTypeface::has_glyph(char32_t c) const { return impl_->index(c) != 0; }

void HdTypeface::prewarm(float cap_px) const {
  const float s = cap_px / impl_->cap_units;
  for (char32_t c = 32; c < 127; ++c) {
    const int gi = impl_->index(c);
    for (int sub = 0; sub < subpixel_steps; ++sub) {
      std::lock_guard lock(impl_->mutex);
      (void)impl_->glyph(gi, c, s, s, sub);
    }
  }
}

float HdTypeface::measure(std::u32string_view text, float cap_px,
                          float tracking_px) const {
  const float scale = cap_px / impl_->cap_units;
  float pen = 0;
  int prev = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const int gi = impl_->index(text[i]);
    if (prev != 0) {
      pen += scale * static_cast<float>(
                         stbtt_GetGlyphKernAdvance(&impl_->info, prev, gi));
    }
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, gi, &advance, &bearing);
    pen += scale * static_cast<float>(advance);
    if (i + 1 < text.size()) {
      pen += tracking_px;
    }
    prev = gi;
  }
  return pen;
}

HdTypeface::Coverage HdTypeface::line(std::u32string_view text, float cap_px,
                                      float tracking_px,
                                      float max_width) const {
  Coverage out;
  if (text.empty() || cap_px <= 0) {
    return out;
  }
  const float sy = cap_px / impl_->cap_units;
  const float natural = measure(text, cap_px, tracking_px);
  // Narrower than the original's line is fine; wider would overlap what is
  // next to it, so it is narrowed (by at most what it takes).
  const float squeeze =
      natural > max_width && max_width > 0 ? max_width / natural : 1.0f;
  const float sx = sy * squeeze;
  const float track = tracking_px * squeeze;

  struct Placed {
    const GlyphBitmap* g;
    int x;
  };
  std::vector<Placed> placed;
  placed.reserve(text.size());
  std::lock_guard lock(impl_->mutex);
  // Trimmed only between lines: the glyphs placed below are referenced
  // until the line is assembled.
  if (impl_->cache.size() + text.size() > max_cached_glyphs) {
    impl_->cache.clear();
  }
  float pen = 0;
  int prev = 0;
  int left = 0;
  int right = 0;
  bool any = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const int gi = impl_->index(text[i]);
    if (prev != 0) {
      pen += sx * static_cast<float>(
                      stbtt_GetGlyphKernAdvance(&impl_->info, prev, gi));
    }
    const float whole = std::floor(pen);
    const int sub = std::clamp(static_cast<int>((pen - whole) * subpixel_steps),
                               0, subpixel_steps - 1);
    const GlyphBitmap& g = impl_->glyph(gi, text[i], sx, sy, sub);
    if (g.w > 0 && g.h > 0) {
      const int x = static_cast<int>(whole) + g.x0;
      placed.push_back({&g, x});
      left = any ? std::min(left, x) : x;
      right = any ? std::max(right, x + g.w) : x + g.w;
      any = true;
    }
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, gi, &advance, &bearing);
    pen += sx * static_cast<float>(advance) + track;
    prev = gi;
  }
  if (!any) {
    return out;
  }
  const int above =
      static_cast<int>(std::ceil(sy * static_cast<float>(impl_->ascent)));
  const int below =
      static_cast<int>(std::ceil(-sy * static_cast<float>(impl_->descent)));
  out.left = left;
  out.width = right - left;
  out.baseline = above;
  out.height = above + below;
  out.pixels.assign(static_cast<std::size_t>(out.width) * out.height, 0);
  for (const auto& p : placed) {
    for (int y = 0; y < p.g->h; ++y) {
      const int oy = out.baseline + p.g->y0 + y;
      if (oy < 0 || oy >= out.height) {
        continue;
      }
      const uint8_t* src =
          p.g->pixels.data() + static_cast<std::size_t>(y) * p.g->w;
      uint8_t* dst = out.pixels.data() +
                     static_cast<std::size_t>(oy) * out.width + (p.x - left);
      for (int x = 0; x < p.g->w; ++x) {
        dst[x] = std::max(dst[x], src[x]);
      }
    }
  }
  return out;
}

namespace {
std::atomic<bool> hd_text_on{true};
} // namespace

void HdTypefaces::set_enabled(bool on) noexcept { hd_text_on = on; }

bool HdTypefaces::enabled() noexcept { return hd_text_on; }

HdTypefaces& HdTypefaces::instance() {
  static HdTypefaces s;
  return s;
}

void HdTypefaces::configure(const std::filesystem::path& folder) {
  std::lock_guard lock(mutex_);
  fonts_.clear();
  if (folder.empty()) {
    return;
  }
  std::ifstream is(folder / "hd_fonts.json");
  if (!is) {
    Logger::info("HD text: no {} , keeping the original fonts",
                 (folder / "hd_fonts.json").string());
    return;
  }
  nlohmann::json table;
  try {
    table = nlohmann::json::parse(is);
  }
  catch (const nlohmann::json::exception& e) {
    Logger::warn("HD text: can't read hd_fonts.json: {}", e.what());
    return;
  }
  for (const auto& [name, entry] : table.items()) {
    if (!entry.is_object() || !entry.contains("file")) {
      continue; // comments and the like
    }
    HdTypeface::Settings s;
    s.file = entry.value("file", std::string{});
    s.size = entry.value("size", 1.0f);
    s.tracking = entry.value("tracking", 0.0f);
    s.uppercase = entry.value("uppercase", false);
    // A file name only: the table can't reach outside its folder.
    if (s.file.empty() || std::filesystem::path(s.file).has_parent_path()) {
      Logger::warn("HD text: bad file name for {}", name);
      continue;
    }
    auto typeface = HdTypeface::load(folder / s.file, s);
    if (!typeface) {
      Logger::warn("HD text: can't load {} for {}", s.file, name);
      continue;
    }
    fonts_[name] = std::move(typeface);
  }
  Logger::info("HD text: {} fonts", fonts_.size());
}

std::shared_ptr<const HdTypeface>
HdTypefaces::get(std::string_view font_name) const {
  std::lock_guard lock(mutex_);
  auto it = fonts_.find(std::string(font_name));
  return it == fonts_.end() ? nullptr : it->second;
}

} // namespace resources
