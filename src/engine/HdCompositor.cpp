/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/HdCompositor.h"

#include <algorithm>
#include <string_view>
#include <thread>
#include "engine/DisplayConstants.h"
#include "engine/Fatal.h"
#include "engine/Logger.h"
#include "engine/fx/MapStore.h"
#include "engine/fx/NormalLight.h"
#include "engine/gfx/HdList.h"

using gfx::HdImage;
using gfx::HdList;

namespace {

// Textures are ARGB8888 (0xAARRGGBB), the order Direct3D, Vulkan, Metal
// and OpenGL take as is: in any other format SDL converts every upload
// through a temporary copy first.
constexpr SDL_PixelFormat gpu_format = SDL_PIXELFORMAT_ARGB8888;

constexpr uint32_t argb(uint32_t r, uint32_t g, uint32_t b,
                        uint32_t a) noexcept {
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// A straight-alpha HD texel (0xRRGGBBAA) as a premultiplied GPU texel.
uint32_t premultiply(uint32_t p) {
  const uint32_t a = gfx::alpha(p);
  if (a == 255) {
    return (p >> 8) | 0xFF000000u;
  }
  if (a == 0) {
    return 0;
  }
  auto m = [a](uint32_t c) { return (c * a + 127) / 255; };
  return argb(m(gfx::red(p)), m(gfx::green(p)), m(gfx::blue(p)), a);
}

// Upload failures are logged, but not one line per texture per frame.
void upload_failed(const char* what) {
  static int count = 0;
  if (count < 5 || count % 1000 == 0) {
    Logger::warn("HD upload of {} failed ({}x): {}", what, count + 1,
                 SDL_GetError());
  }
  ++count;
}

} // namespace

bool hd_gpu_compositing() {
  static const bool on = [] {
    const char* v = SDL_getenv("PLEGACY_CPU_COMPOSE");
    return !(v && v[0] == '1');
  }();
  return on;
}

HdCompositor::HdCompositor(SDL_Renderer* renderer) : renderer_(renderer) {
  // SDL's software renderer rasterizes triangle lists itself and doesn't
  // blend premultiplied alpha there; it gets one copy per quad instead (it
  // has no draw-call cost to save anyway).
  const char* name = SDL_GetRendererName(renderer_);
  batch_geometry_ = !(name && std::string_view(name) == SDL_SOFTWARE_RENDERER);
  const SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
  const auto max = SDL_GetNumberProperty(
      props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
  if (max > 0) {
    max_texture_size_ = static_cast<int>(max);
  }
}

HdCompositor::~HdCompositor() {
  for (auto& [ptr, e] : cache_) {
    if (e.page < 0) {
      SDL_DestroyTexture(e.tex);
    }
  }
  for (Page& page : pages_) {
    if (page.tex) {
      SDL_DestroyTexture(page.tex);
    }
  }
  if (base_) {
    SDL_DestroyTexture(base_);
  }
}

std::size_t HdCompositor::texture_count() const noexcept {
  std::size_t n = 0;
  for (const auto& [ptr, e] : cache_) {
    n += e.page < 0 ? 1 : 0;
  }
  return n + atlas_pages();
}

std::size_t HdCompositor::atlas_pages() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      pages_, [](const Page& p) { return p.tex != nullptr; }));
}

void HdCompositor::size_atlas() {
  if (page_w_ != 0) {
    return;
  }
  // Wide enough for a line of text across the whole screen.
  const int needed = DisplayConstants::SCREEN_WIDTH * gfx::hd_scale() + 2;
  int w = 256;
  while (w < needed && w < max_texture_size_) {
    w *= 2;
  }
  page_w_ = std::min(w, max_texture_size_);
  page_h_ = std::min(page_w_ / 2, max_texture_size_);
}

int HdCompositor::add_page() {
  size_atlas();
  // In the place of one given back if there is one.
  auto spare = std::ranges::find_if(
      pages_, [](const Page& p) { return p.tex == nullptr; });
  if (spare == pages_.end()) {
    spare = pages_.insert(pages_.end(), Page{});
  }
  spare->tex = SDL_CreateTexture(renderer_, gpu_format,
                                 SDL_TEXTUREACCESS_STATIC, page_w_, page_h_);
  if (!spare->tex) {
    Logger::warn("Cannot create an HD atlas page: {}", SDL_GetError());
    return -1;
  }
  SDL_SetTextureBlendMode(spare->tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  spare->texels.assign(static_cast<std::size_t>(page_w_) * page_h_, 0);
  spare->shelves.clear();
  spare->bottom = 0;
  spare->live = 0;
  spare->dirty_y0 = 0;
  spare->dirty_y1 = 0;
  bytes_ += static_cast<std::size_t>(page_w_) * page_h_ * 4;
  return static_cast<int>(spare - pages_.begin());
}

void HdCompositor::prepare_atlas() {
  if (std::ranges::none_of(pages_,
                           [](const Page& p) { return p.tex != nullptr; })) {
    (void)add_page();
  }
}

HdCompositor::Entry*
HdCompositor::create_in_atlas(const std::shared_ptr<const HdImage>& img) {
  size_atlas();
  // A transparent texel all round, so even smooth scaling never picks up
  // the neighbours.
  const int w = img->width + 2;
  const int h = img->height + 2;
  if (w > page_w_ || h > page_h_ / 4) {
    return nullptr;
  }
  // The flattest shelf it fits on without wasting more than a third of the
  // shelf's height, else a new shelf, else a new page.
  int index = -1;
  Shelf* shelf = nullptr;
  for (std::size_t i = 0; i < pages_.size() && index < 0; ++i) {
    Page& page = pages_[i];
    if (!page.tex) {
      continue;
    }
    for (Shelf& s : page.shelves) {
      if (s.h >= h && s.h * 2 <= h * 3 && s.x + w <= page_w_ &&
          (!shelf || s.h < shelf->h)) {
        shelf = &s;
      }
    }
    if (shelf || page.bottom + h <= page_h_) {
      index = static_cast<int>(i);
    }
  }
  if (index < 0) {
    index = add_page();
    if (index < 0) {
      return nullptr;
    }
  }
  Page& page = pages_[static_cast<std::size_t>(index)];
  if (!shelf) {
    page.shelves.push_back({page.bottom, h, 0});
    page.bottom += h;
    shelf = &page.shelves.back();
  }
  Entry e;
  e.img = img;
  e.tex = page.tex;
  e.last_used = frame_;
  e.page = index;
  e.x = shelf->x + 1;
  e.y = shelf->y + 1;
  shelf->x += w;
  ++page.live;
  return &cache_.insert_or_assign(img.get(), std::move(e)).first->second;
}

HdCompositor::Entry*
HdCompositor::create(const std::shared_ptr<const HdImage>& img) {
  if (img->atlas) {
    if (Entry* e = create_in_atlas(img)) {
      return e;
    }
  }
  if (img->width > max_texture_size_ || img->height > max_texture_size_ ||
      img->width <= 0 || img->height <= 0) {
    Logger::warn("HD picture {}x{} is larger than the GPU allows ({})",
                 img->width, img->height, max_texture_size_);
    return nullptr;
  }
  SDL_Texture* tex = SDL_CreateTexture(
      renderer_, gpu_format, SDL_TEXTUREACCESS_STATIC, img->width, img->height);
  if (!tex) {
    Logger::warn("Cannot create HD texture: {}", SDL_GetError());
    return nullptr;
  }
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
  Entry e;
  e.img = img;
  e.tex = tex;
  e.bytes = img->pixels.size() * 4;
  e.last_used = frame_;
  bytes_ += e.bytes;
  return &cache_.insert_or_assign(img.get(), std::move(e)).first->second;
}

void HdCompositor::upload_rows(Entry& e, const HdImage& img, int rows) {
  if (e.page >= 0) {
    // Small: all at once, with the transparent frame round it, into the
    // page's texels; flush_atlas() sends them.
    Page& page = pages_[static_cast<std::size_t>(e.page)];
    const int w = img.width + 2;
    uint32_t* top = page.texels.data() +
                    static_cast<std::size_t>(e.y - 1) * page_w_ + (e.x - 1);
    std::fill_n(top, w, 0u);
    for (int y = 0; y < img.height; ++y) {
      uint32_t* row = top + static_cast<std::size_t>(y + 1) * page_w_;
      row[0] = 0;
      std::transform(img.row(y), img.row(y) + img.width, row + 1, premultiply);
      row[w - 1] = 0;
    }
    std::fill_n(top + static_cast<std::size_t>(img.height + 1) * page_w_, w,
                0u);
    const int x0 = e.x - 1;
    const int x1 = e.x + img.width + 1;
    const int y0 = e.y - 1;
    const int y1 = e.y + img.height + 1;
    if (page.dirty_y0 == page.dirty_y1) {
      page.dirty_x0 = x0;
      page.dirty_x1 = x1;
      page.dirty_y0 = y0;
      page.dirty_y1 = y1;
    }
    else {
      page.dirty_x0 = std::min(page.dirty_x0, x0);
      page.dirty_x1 = std::max(page.dirty_x1, x1);
      page.dirty_y0 = std::min(page.dirty_y0, y0);
      page.dirty_y1 = std::max(page.dirty_y1, y1);
    }
    e.rows_done = img.height;
    atlas_bytes_ += static_cast<std::size_t>(w) * (img.height + 2) * 4;
    ++atlas_pictures_;
    return;
  }
  const int y0 = e.rows_done;
  const int y1 = std::min(img.height, y0 + rows);
  if (y1 <= y0) {
    return;
  }
  const std::size_t first = static_cast<std::size_t>(y0) * img.width;
  const std::size_t n = static_cast<std::size_t>(y1 - y0) * img.width;
  scratch_.resize(n);
  // Big bands (a whole 4K screen is 9 million texels) use several cores.
  const unsigned workers =
      n < 1'000'000
          ? 1u
          : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
  {
    std::vector<std::jthread> pool;
    const std::size_t step = (n + workers - 1) / workers;
    auto part = [&](std::size_t a, std::size_t b) {
      std::transform(
          img.pixels.begin() + static_cast<std::ptrdiff_t>(first + a),
          img.pixels.begin() + static_cast<std::ptrdiff_t>(first + b),
          scratch_.begin() + static_cast<std::ptrdiff_t>(a), premultiply);
    };
    for (unsigned w = 1; w < workers; ++w) {
      const std::size_t a = std::min(n, w * step);
      pool.emplace_back(part, a, std::min(n, a + step));
    }
    part(0, std::min(n, step));
  }
  const SDL_Rect rect{0, y0, img.width, y1 - y0};
  if (!SDL_UpdateTexture(e.tex, &rect, scratch_.data(), img.width * 4)) {
    // Not marked done: tried again when next drawn or streamed, and until
    // then the original pixels show (see locate).
    upload_failed("a picture");
    return;
  }
  e.rows_done = y1;
}

void HdCompositor::flush_atlas() {
  for (Page& page : pages_) {
    if (!page.tex || page.dirty_y0 == page.dirty_y1) {
      continue;
    }
    const uint64_t started = SDL_GetPerformanceCounter();
    const SDL_Rect rect{page.dirty_x0, page.dirty_y0,
                        page.dirty_x1 - page.dirty_x0,
                        page.dirty_y1 - page.dirty_y0};
    sent_bytes_ += static_cast<std::size_t>(rect.w) * rect.h * 4;
    if (SDL_UpdateTexture(page.tex, &rect,
                          page.texels.data() +
                              static_cast<std::size_t>(rect.y) * page_w_ +
                              rect.x,
                          page_w_ * 4)) {
      page.dirty_y0 = page.dirty_y1 = 0;
    }
    else {
      // Kept dirty: its pictures aren't drawn (see locate) and the upload
      // is tried again on the next flush.
      upload_failed("an atlas page");
    }
    upload_ms_ += static_cast<double>(SDL_GetPerformanceCounter() - started) *
                  1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
  }
}

HdCompositor::Placed
HdCompositor::place(const std::shared_ptr<const HdImage>& img) {
  (void)locate(img);
  flush_atlas();
  return locate(img);
}

bool HdCompositor::ready(const Entry& e, const HdImage& img) const {
  if (e.rows_done < img.height) {
    return false;
  }
  if (e.page < 0) {
    return true;
  }
  // In an atlas page: once the page's changes are on the GPU.
  const Page& page = pages_[static_cast<std::size_t>(e.page)];
  return page.dirty_y0 == page.dirty_y1;
}

HdCompositor::Placed
HdCompositor::locate(const std::shared_ptr<const HdImage>& img) {
  Entry* e = nullptr;
  auto it = cache_.find(img.get());
  if (it != cache_.end()) {
    if (it->second.img.lock() == img) {
      e = &it->second;
    }
    else {
      // Same address, new picture: the old one is gone.
      evict(it->second);
      cache_.erase(it);
    }
  }
  if (e && e->rows_done >= img->height) {
    e->last_used = frame_;
    if (!ready(*e, *img)) {
      return {};
    }
    return {e->tex, e->x, e->y};
  }
  // Not there yet, or only partly streamed: finish it now.
  const uint64_t started = SDL_GetPerformanceCounter();
  if (!e) {
    e = create(img);
    if (!e) {
      return {};
    }
  }
  upload_rows(*e, *img, img->height);
  e->last_used = frame_;
  upload_ms_ += static_cast<double>(SDL_GetPerformanceCounter() - started) *
                1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
  if (!ready(*e, *img)) {
    return {};
  }
  return {e->tex, e->x, e->y};
}

void HdCompositor::stream(
    const std::vector<std::shared_ptr<const HdImage>>& add, double budget_ms) {
  for (const auto& img : add) {
    if (img) {
      stream_queue_.emplace_back(img);
    }
  }
  const uint64_t freq = SDL_GetPerformanceFrequency();
  const uint64_t started = SDL_GetPerformanceCounter();
  auto spent_ms = [&] {
    return static_cast<double>(SDL_GetPerformanceCounter() - started) * 1000.0 /
           static_cast<double>(freq);
  };
  std::size_t done = 0;
  while (done < stream_queue_.size() && spent_ms() < budget_ms) {
    const auto img = stream_queue_[done].lock();
    if (!img) {
      ++done;
      continue;
    }
    Entry* e = nullptr;
    auto it = cache_.find(img.get());
    if (it != cache_.end() && it->second.img.lock() == img) {
      e = &it->second;
    }
    else {
      if (it != cache_.end()) {
        evict(it->second);
        cache_.erase(it);
      }
      e = create(img);
    }
    if (!e || e->rows_done >= img->height) {
      if (maps_wanted_.erase(img.get()) > 0 && e) {
        ++map_uploads_; // on the GPU: drawable from the next frame
      }
      ++done;
      continue;
    }
    // About a million texels (4 MB) per band.
    const int band = std::max(16, 1'000'000 / std::max(1, img->width));
    const int before = e->rows_done;
    upload_rows(*e, *img, band);
    if (e->rows_done == before) {
      // Failed: left to be finished when it is drawn (a map is asked for
      // again by its next map_texture()).
      maps_wanted_.erase(img.get());
      ++done;
    }
  }
  stream_queue_.erase(stream_queue_.begin(),
                      stream_queue_.begin() +
                          static_cast<std::ptrdiff_t>(done));
  if (stream_queue_.empty()) {
    maps_wanted_.clear(); // also forgets maps gone before their upload
  }
  flush_atlas();
}

HdCompositor::Placed
HdCompositor::map_texture(const std::shared_ptr<const HdImage>& map) {
  if (!map) {
    return {};
  }
  if (fx::MapStore::instance().blocking()) {
    return place(map);
  }
  const auto it = cache_.find(map.get());
  if (it != cache_.end() && it->second.img.lock() == map &&
      ready(it->second, *map)) {
    it->second.last_used = frame_;
    return {it->second.tex, it->second.x, it->second.y};
  }
  if (maps_wanted_.insert(map.get()).second) {
    stream_queue_.emplace_back(map);
  }
  return {};
}

std::size_t
HdCompositor::preload(const std::vector<std::shared_ptr<const HdImage>>& imgs,
                      std::size_t budget_bytes) {
  std::size_t done = 0;
  for (const auto& img : imgs) {
    if (done >= budget_bytes) {
      break;
    }
    auto it = cache_.find(img.get());
    if (it != cache_.end() && it->second.img.lock() == img) {
      continue;
    }
    if (texture_for(img)) {
      done += img->pixels.size() * 4;
    }
  }
  return done;
}

void HdCompositor::evict(Entry& e) {
  if (e.page >= 0) {
    Page& page = pages_[static_cast<std::size_t>(e.page)];
    if (--page.live == 0) {
      // Empty: filled again from the top. Pages beyond the first are given
      // back to the GPU.
      page.shelves.clear();
      page.bottom = 0;
      if (e.page > 0) {
        SDL_DestroyTexture(page.tex);
        page.tex = nullptr;
        page.texels = {};
        bytes_ -= static_cast<std::size_t>(page_w_) * page_h_ * 4;
      }
    }
    e.tex = nullptr;
    e.page = -1;
    return;
  }
  SDL_DestroyTexture(e.tex);
  bytes_ -= e.bytes;
  e.tex = nullptr;
}

void HdCompositor::end_frame() {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.img.expired()) {
      evict(it->second);
      it = cache_.erase(it);
    }
    else {
      ++it;
    }
  }
  if (bytes_ > budget_) {
    std::vector<std::pair<uint64_t, const HdImage*>> lru;
    for (const auto& [ptr, e] : cache_) {
      // Atlas pictures free no memory one by one; they go when unused.
      if (e.last_used < frame_ && e.page < 0) {
        lru.emplace_back(e.last_used, ptr);
      }
    }
    std::sort(lru.begin(), lru.end());
    for (const auto& [used, ptr] : lru) {
      if (bytes_ <= budget_) {
        break;
      }
      auto it = cache_.find(ptr);
      evict(it->second);
      cache_.erase(it);
    }
  }
  ++frame_;
}

void HdCompositor::upload_base(const gfx::Surface& layer, float fade) {
  const int w = layer.width();
  const int h = layer.height();
  // One texture for every layer, grown when a larger one comes: layers of
  // different sizes (panels, popups) in one frame then cost an upload of
  // their own rect, not a new texture each.
  if (!base_ || base_w_ < w || base_h_ < h) {
    if (base_) {
      SDL_DestroyTexture(base_);
    }
    base_w_ = std::max({base_w_, w, DisplayConstants::SCREEN_WIDTH});
    base_h_ = std::max({base_h_, h, DisplayConstants::SCREEN_HEIGHT});
    base_ = SDL_CreateTexture(renderer_, gpu_format,
                              SDL_TEXTUREACCESS_STREAMING, base_w_, base_h_);
    if (!base_) {
      fatal(SDL_GetError());
    }
    SDL_SetTextureScaleMode(base_, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(base_, SDL_BLENDMODE_NONE);
  }
  // Index 0 is the transparent colour key; everything else is opaque, so
  // premultiplied and straight alpha are the same here.
  uint32_t lut[256];
  std::fill(std::begin(lut), std::end(lut), argb(0, 0, 0, 255));
  const auto& pal = layer.colors();
  for (std::size_t i = 0; i < pal.size() && i < 256; ++i) {
    lut[i] =
        argb(static_cast<uint32_t>(static_cast<float>(pal[i].r) * fade),
             static_cast<uint32_t>(static_cast<float>(pal[i].g) * fade),
             static_cast<uint32_t>(static_cast<float>(pal[i].b) * fade), 255);
  }
  lut[0] = 0;
  const auto px = layer.pixels();
  scratch_.resize(px.size());
  for (std::size_t i = 0; i < px.size(); ++i) {
    scratch_[i] = lut[px[i]];
  }
  const SDL_Rect rect{0, 0, w, h};
  if (!SDL_UpdateTexture(base_, &rect, scratch_.data(), w * 4)) {
    upload_failed("a layer's pixels");
  }
}

void HdCompositor::draw_runs(const std::vector<Run>& runs, std::size_t& pos,
                             uint32_t floor, int k) {
  // Rect copies, not a triangle list: exact on every backend (texture
  // coordinates at texel edges round differently between rasterizers), and
  // SDL batches consecutive copies of one texture into one draw anyway.
  const auto fk = static_cast<float>(k);
  for (; pos < runs.size() && runs[pos].floor == floor; ++pos) {
    const Run& r = runs[pos];
    const SDL_FRect src{static_cast<float>(r.x0), static_cast<float>(r.y),
                        static_cast<float>(r.x1 - r.x0), 1.0f};
    const SDL_FRect dst{src.x * fk, src.y * fk, src.w * fk, fk};
    SDL_RenderTexture(renderer_, base_, &src, &dst);
    ++stats_.base_runs;
  }
}

void HdCompositor::draw_layer(SDL_Texture* target, const gfx::Surface& layer,
                              float fade, int art_from_x) {
  stats_ = {};
  normal_lit_ = 0;
  const int k = gfx::hd_scale();
  const int w = layer.width();
  const int h = layer.height();
  upload_base(layer, fade);
  SDL_SetRenderTarget(renderer_, target);

  // 1. The indexed pixels, everywhere (replacing whatever was there).
  const SDL_FRect src_all{0, 0, static_cast<float>(w), static_cast<float>(h)};
  const SDL_FRect dst_all{0, 0, static_cast<float>(w * k),
                          static_cast<float>(h * k)};
  SDL_RenderTexture(renderer_, base_, &src_all, &dst_all);

  const HdList* list = layer.hd_list();
  if (!list) {
    return;
  }

  // 2. Runs of indexed pixels that must be redrawn just before command f,
  //    because a plain write or a soft-edged replacement hid the commands
  //    before f there (see HdList.h).
  runs_.clear();
  if (!list->is_leaf()) {
    for (int y = 0; y < h; ++y) {
      int x = 0;
      while (x < w) {
        const uint32_t f = list->floor_of(x, y);
        if (f == 0 || !list->needs_base(x, y)) {
          ++x;
          continue;
        }
        const int x0 = x;
        while (x < w && list->floor_of(x, y) == f && list->needs_base(x, y)) {
          ++x;
        }
        runs_.push_back({f, y, x0, x});
      }
    }
    std::stable_sort(
        runs_.begin(), runs_.end(),
        [](const Run& a, const Run& b) { return a.floor < b.floor; });
  }

  // 3. The pictures, in order, with those runs in between. New lines of
  //    text first, all of them, so each atlas page is sent once.
  const auto& cmds = list->cmds();
  for (const gfx::HdCmd& c : cmds) {
    if (c.img && c.img->atlas) {
      (void)locate(c.img);
    }
  }
  flush_atlas();
  std::size_t pos = 0;
  batch_tex_ = nullptr;
  // The 1994 look left of art_from_x: art is cut to the right of it, text
  // (tinted letters, atlas lines) is not.
  const bool no_art = art_from_x >= w * k;
  const bool split = art_from_x > 0 && !no_art;
  const SDL_Rect art_clip{art_from_x, 0, w * k - art_from_x, h * k};
  bool clipped = false;
  const auto clip_art = [&](bool on) {
    if (on != clipped) {
      flush_batch();
      SDL_SetRenderClipRect(renderer_, on ? &art_clip : nullptr);
      clipped = on;
    }
  };
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    if (pos < runs_.size() && runs_[pos].floor == i) {
      flush_batch();
      draw_runs(runs_, pos, static_cast<uint32_t>(i), k);
    }
    const gfx::HdCmd& c = cmds[i];
    const bool text = c.tint != 0 || (c.img && c.img->atlas);
    if (no_art && !text) {
      continue;
    }
    if (split) {
      clip_art(!text);
    }
    const Placed at = locate(c.img);
    SDL_Texture* tex = at.tex;
    if (!tex) {
      continue;
    }
    if (normal_pass_ && normal_lights_ && !text && !c.img->atlas &&
        normal_pass_->ready()) {
      if (auto map = fx::MapStore::instance().normal(c.img.get())) {
        const Placed nm = map_texture(map);
        if (nm.tex) {
          flush_batch();
          const SDL_FRect src{
              static_cast<float>(c.sx + at.x), static_cast<float>(c.sy + at.y),
              static_cast<float>(c.sw), static_cast<float>(c.sh)};
          const SDL_FRect dst{
              static_cast<float>(c.dx), static_cast<float>(c.dy),
              static_cast<float>(c.dw()), static_cast<float>(c.dh())};
          SDL_SetTextureColorModFloat(tex, fade, fade, fade);
          SDL_SetTextureScaleMode(tex, c.half ? SDL_SCALEMODE_LINEAR
                                              : SDL_SCALEMODE_NEAREST);
          if (normal_pass_->draw(tex, nm.tex, src, dst, *normal_lights_)) {
            ++normal_lit_;
            ++stats_.quads;
            continue;
          }
        }
      }
    }
    if (!c.half && batch_geometry_) {
      // 1:1 copies (sprites, text, letters) go into one triangle list per
      // texture: a screen of text is hundreds of letters, and a draw call
      // each was the biggest cost on Direct3D.
      if (tex != batch_tex_) {
        flush_batch();
        batch_tex_ = tex;
      }
      const SDL_FColor colour =
          c.tint != 0
              ? SDL_FColor{fade * static_cast<float>(gfx::red(c.tint)) / 255.0f,
                           fade * static_cast<float>(gfx::green(c.tint)) /
                               255.0f,
                           fade * static_cast<float>(gfx::blue(c.tint)) /
                               255.0f,
                           1.0f}
              : SDL_FColor{fade, fade, fade, 1.0f};
      // The same corners and texture coordinates SDL_RenderTexture makes.
      const auto tw = static_cast<float>(tex->w);
      const auto th = static_cast<float>(tex->h);
      const float u0 = static_cast<float>(c.sx + at.x) / tw;
      const float v0 = static_cast<float>(c.sy + at.y) / th;
      const float u1 = static_cast<float>(c.sx + at.x + c.sw) / tw;
      const float v1 = static_cast<float>(c.sy + at.y + c.sh) / th;
      const auto x0 = static_cast<float>(c.dx);
      const auto y0 = static_cast<float>(c.dy);
      const auto x1 = static_cast<float>(c.dx + c.dw());
      const auto y1 = static_cast<float>(c.dy + c.dh());
      const auto first = static_cast<int>(batch_vertices_.size());
      batch_vertices_.push_back({{x0, y0}, colour, {u0, v0}});
      batch_vertices_.push_back({{x1, y0}, colour, {u1, v0}});
      batch_vertices_.push_back({{x1, y1}, colour, {u1, v1}});
      batch_vertices_.push_back({{x0, y1}, colour, {u0, v1}});
      for (int v : {0, 1, 2, 0, 2, 3}) {
        batch_indices_.push_back(first + v);
      }
      ++stats_.quads;
      continue;
    }
    flush_batch();
    if (c.tint != 0) {
      // A coverage mask (white): the colour is the GPU's modulation.
      SDL_SetTextureColorModFloat(
          tex, fade * static_cast<float>(gfx::red(c.tint)) / 255.0f,
          fade * static_cast<float>(gfx::green(c.tint)) / 255.0f,
          fade * static_cast<float>(gfx::blue(c.tint)) / 255.0f);
    }
    else {
      SDL_SetTextureColorModFloat(tex, fade, fade, fade);
    }
    SDL_SetTextureScaleMode(tex, c.half ? SDL_SCALEMODE_LINEAR
                                        : SDL_SCALEMODE_NEAREST);
    const SDL_FRect src{static_cast<float>(c.sx + at.x),
                        static_cast<float>(c.sy + at.y),
                        static_cast<float>(c.sw), static_cast<float>(c.sh)};
    const SDL_FRect dst{static_cast<float>(c.dx), static_cast<float>(c.dy),
                        static_cast<float>(c.dw()), static_cast<float>(c.dh())};
    SDL_RenderTexture(renderer_, tex, &src, &dst);
    ++stats_.quads;
  }
  clip_art(false);
  flush_batch();
  draw_runs(runs_, pos, static_cast<uint32_t>(cmds.size()), k);
}

bool HdCompositor::has_emissive(const gfx::Surface& layer) const {
  const gfx::HdList* list = layer.hd_list();
  fx::MapStore& maps = fx::MapStore::instance();
  if (!list || !maps.enabled()) {
    return false;
  }
  return std::ranges::any_of(list->cmds(), [&maps](const gfx::HdCmd& c) {
    return c.img && c.tint == 0 && !c.img->atlas &&
           maps.emissive(c.img.get()) != nullptr;
  });
}

bool HdCompositor::draw_emissive(SDL_Texture* target, const gfx::Surface& layer,
                                 int art_from_x) {
  const gfx::HdList* list = layer.hd_list();
  fx::MapStore& maps = fx::MapStore::instance();
  if (!list || !maps.enabled()) {
    return false;
  }
  const auto& cmds = list->cmds();
  auto glow_of = [&maps](const gfx::HdCmd& c) {
    return c.img && c.tint == 0 && !c.img->atlas ? maps.emissive(c.img.get())
                                                 : nullptr;
  };
  std::size_t first = cmds.size();
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    if (glow_of(cmds[i])) {
      first = i;
      break;
    }
  }
  if (first == cmds.size()) {
    return false;
  }
  const int k = gfx::hd_scale();
  const int w = layer.width() * k;
  const int h = layer.height() * k;
  SDL_SetRenderTarget(renderer_, target);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
  SDL_RenderClear(renderer_);
  if (art_from_x >= w) {
    return true;
  }
  const SDL_Rect clip{art_from_x, 0, w - art_from_x, h};
  if (art_from_x > 0) {
    SDL_SetRenderClipRect(renderer_, &clip);
  }
  auto draw = [this](SDL_Texture* tex, const gfx::HdCmd& c, int ox, int oy,
                     float mod, SDL_BlendMode mode) {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    SDL_BlendMode old = SDL_BLENDMODE_NONE;
    SDL_GetTextureColorModFloat(tex, &r, &g, &b);
    SDL_GetTextureBlendMode(tex, &old);
    SDL_SetTextureColorModFloat(tex, mod, mod, mod);
    SDL_SetTextureBlendMode(tex, mode);
    SDL_SetTextureScaleMode(tex, c.half ? SDL_SCALEMODE_LINEAR
                                        : SDL_SCALEMODE_NEAREST);
    const SDL_FRect src{static_cast<float>(c.sx + ox),
                        static_cast<float>(c.sy + oy), static_cast<float>(c.sw),
                        static_cast<float>(c.sh)};
    const SDL_FRect dst{static_cast<float>(c.dx), static_cast<float>(c.dy),
                        static_cast<float>(c.dw()), static_cast<float>(c.dh())};
    SDL_RenderTexture(renderer_, tex, &src, &dst);
    SDL_SetTextureColorModFloat(tex, r, g, b);
    SDL_SetTextureBlendMode(tex, old);
  };
  for (std::size_t i = first; i < cmds.size(); ++i) {
    const gfx::HdCmd& c = cmds[i];
    if (!c.img || c.tint != 0 || c.img->atlas) {
      continue; // text gives off no light and hides little
    }
    const Placed at = locate(c.img);
    if (!at.tex) {
      continue;
    }
    // The picture hides the glow under it, as far as it is opaque...
    if (i > first) {
      draw(at.tex, c, at.x, at.y, 0.0f, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
    // ...and adds its own.
    if (auto glow = glow_of(c)) {
      const Placed g = map_texture(glow);
      if (g.tex) {
        draw(g.tex, c, g.x, g.y, 1.0f, SDL_BLENDMODE_ADD_PREMULTIPLIED);
      }
    }
  }
  if (art_from_x > 0) {
    SDL_SetRenderClipRect(renderer_, nullptr);
  }
  return true;
}

void HdCompositor::flush_batch() {
  if (batch_tex_ && !batch_indices_.empty()) {
    // The colour (fade, text colour) is in the vertices.
    SDL_SetTextureColorModFloat(batch_tex_, 1.0f, 1.0f, 1.0f);
    SDL_SetTextureScaleMode(batch_tex_, SDL_SCALEMODE_NEAREST);
    SDL_RenderGeometry(renderer_, batch_tex_, batch_vertices_.data(),
                       static_cast<int>(batch_vertices_.size()),
                       batch_indices_.data(),
                       static_cast<int>(batch_indices_.size()));
    ++stats_.batches;
  }
  batch_vertices_.clear();
  batch_indices_.clear();
  batch_tex_ = nullptr;
}
