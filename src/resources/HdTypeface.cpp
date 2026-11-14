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
#include <stop_token>
#include <thread>
#include <utility>
#include "engine/Logger.h"
#include "engine/gfx/HdImage.h"

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
// How much closer than the font's own spacing letters may move to fit a
// line into the original's width, as a fraction of the cap height.
constexpr float tight_tracking = 0.06f;
// The letters in a glyph sheet: printable ASCII.
constexpr char32_t sheet_first = 32;
constexpr char32_t sheet_last = 127;
constexpr int sheet_slot(char32_t c, int sub) {
  return static_cast<int>(c - sheet_first) * subpixel_steps + sub;
}
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
  // Glyph sheets by scale (float bits), and the pictures of those not yet
  // handed out for upload.
  std::unordered_map<uint32_t, std::shared_ptr<const Sheet>> sheets;
  std::vector<std::shared_ptr<const gfx::HdImage>> new_sheets;
  // Background prewarms (see prewarm). Declared last: destroyed (stopped
  // and joined) before everything they use.
  std::vector<std::jthread> workers;

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

  // Makes the glyph sheet of scale s from the glyph cache. Call with the
  // mutex held.
  void build_sheet(float s) {
    const uint32_t key = float_bits(s);
    if (sheets.contains(key)) {
      return;
    }
    std::vector<const GlyphBitmap*> glyphs(
        static_cast<std::size_t>((sheet_last - sheet_first) * subpixel_steps));
    int widest = 0;
    for (char32_t c = sheet_first; c < sheet_last; ++c) {
      for (int sub = 0; sub < subpixel_steps; ++sub) {
        const GlyphBitmap& g = glyph(index(c), c, s, s, sub);
        glyphs[static_cast<std::size_t>(sheet_slot(c, sub))] = &g;
        widest = std::max(widest, g.w);
      }
    }
    // Rows of letters, a transparent texel between them and round the edge
    // so smooth scaling never mixes neighbours.
    const int width = std::max(1024, widest + 2);
    auto sheet = std::make_shared<Sheet>();
    sheet->slots.resize(glyphs.size());
    int x = 1;
    int y = 1;
    int row = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      const GlyphBitmap& g = *glyphs[i];
      if (g.w <= 0 || g.h <= 0) {
        continue;
      }
      if (x + g.w + 1 > width) {
        x = 1;
        y += row + 1;
        row = 0;
      }
      sheet->slots[i] = Slot{x, y, g.w, g.h};
      x += g.w + 1;
      row = std::max(row, g.h);
    }
    auto image = std::make_shared<gfx::HdImage>(width, y + row + 1, 0u);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      const GlyphBitmap& g = *glyphs[i];
      const Slot& slot = sheet->slots[i];
      for (int gy = 0; gy < slot.h; ++gy) {
        const uint8_t* src =
            g.pixels.data() + static_cast<std::size_t>(gy) * g.w;
        uint32_t* dst = image->row(slot.y + gy) + slot.x;
        for (int gx = 0; gx < slot.w; ++gx) {
          dst[gx] = src[gx] != 0 ? gfx::pack_rgba(255, 255, 255, src[gx]) : 0u;
        }
      }
    }
    sheet->image = image;
    new_sheets.push_back(std::move(image));
    sheets.emplace(key, std::move(sheet));
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
  // On a thread of its own, one glyph at a time, so a line being drawn
  // meanwhile only ever waits for one glyph.
  Impl* impl = impl_.get();
  const float s = cap_px / impl->cap_units;
  std::lock_guard lock(impl->mutex);
  impl->workers.emplace_back([impl, s](std::stop_token stop) {
    for (char32_t c = sheet_first; c < sheet_last; ++c) {
      const int gi = impl->index(c);
      for (int sub = 0; sub < subpixel_steps; ++sub) {
        if (stop.stop_requested()) {
          return;
        }
        std::lock_guard glyph_lock(impl->mutex);
        (void)impl->glyph(gi, c, s, s, sub);
      }
    }
    std::lock_guard sheet_lock(impl->mutex);
    impl->build_sheet(s);
  });
}

void HdTypeface::wait_for_prewarm() const {
  // Taken out under the lock, joined without it: a worker takes the lock
  // for every glyph.
  std::vector<std::jthread> done;
  {
    std::lock_guard lock(impl_->mutex);
    done.swap(impl_->workers);
  }
  for (auto& worker : done) {
    worker.join();
  }
}

std::shared_ptr<const HdTypeface::Sheet> HdTypeface::sheet(float cap_px) const {
  std::lock_guard lock(impl_->mutex);
  const auto it = impl_->sheets.find(float_bits(cap_px / impl_->cap_units));
  return it == impl_->sheets.end() ? nullptr : it->second;
}

void HdTypeface::take_new_sheets(
    std::vector<std::shared_ptr<const gfx::HdImage>>& out) const {
  std::lock_guard lock(impl_->mutex);
  for (auto& image : impl_->new_sheets) {
    out.push_back(std::move(image));
  }
  impl_->new_sheets.clear();
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
  // next to it. Then the letters first move closer together, by up to
  // tight_tracking of the cap height between two letters, which keeps their
  // shapes (and lets the line be drawn from the glyph sheet); only a line
  // still too wide is narrowed, by at most what it takes.
  float track = tracking_px;
  float squeeze = 1.0f;
  if (max_width > 0 && natural > max_width) {
    const auto gaps = static_cast<float>(text.size() - 1);
    const float tightest = std::min(tracking_px, -tight_tracking * cap_px);
    const float at_tightest = natural - (tracking_px - tightest) * gaps;
    if (gaps > 0 && at_tightest <= max_width) {
      track = tracking_px - (natural - max_width) / gaps;
    }
    else {
      squeeze = max_width / (gaps > 0 ? at_tightest : natural);
      track = (gaps > 0 ? tightest : tracking_px) * squeeze;
    }
  }
  const float sx = sy * squeeze;

  struct Placed {
    const GlyphBitmap* g;
    int x;
    int slot; // in the glyph sheet, or -1
  };
  bool from_sheet = squeeze == 1.0f;
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
      const bool in_sheet = text[i] >= sheet_first && text[i] < sheet_last;
      from_sheet = from_sheet && in_sheet;
      placed.push_back({&g, x, in_sheet ? sheet_slot(text[i], sub) : -1});
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
  out.from_sheet = from_sheet;
  out.squeeze = squeeze;
  if (from_sheet) {
    out.glyphs.reserve(placed.size());
    for (const auto& p : placed) {
      out.glyphs.push_back({p.slot, p.x - left, out.baseline + p.g->y0});
    }
  }
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
std::atomic<bool> sheets_on{true};
} // namespace

void HdTypefaces::set_enabled(bool on) noexcept { hd_text_on = on; }

bool HdTypefaces::enabled() noexcept { return hd_text_on; }

void HdTypefaces::set_sheets_enabled(bool on) noexcept { sheets_on = on; }

bool HdTypefaces::sheets_enabled() noexcept { return sheets_on; }

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

std::vector<std::shared_ptr<const gfx::HdImage>>
HdTypefaces::take_new_sheets() const {
  std::vector<std::shared_ptr<const HdTypeface>> all;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [name, typeface] : fonts_) {
      all.push_back(typeface);
    }
  }
  std::vector<std::shared_ptr<const gfx::HdImage>> out;
  for (const auto& typeface : all) {
    typeface->take_new_sheets(out);
  }
  return out;
}

void HdTypefaces::wait_for_prewarm() const {
  std::vector<std::shared_ptr<const HdTypeface>> all;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [name, typeface] : fonts_) {
      all.push_back(typeface);
    }
  }
  for (const auto& typeface : all) {
    typeface->wait_for_prewarm();
  }
}

} // namespace resources
