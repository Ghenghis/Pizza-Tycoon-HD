/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/MapStore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "engine/Logger.h"
#include "engine/gfx/Png.h"
#include "resources/HdPack.h"

namespace fx {

MapStore& MapStore::instance() {
  static MapStore store;
  return store;
}

MapStore::~MapStore() { stop_worker(); }

void MapStore::loading(Loading mode) { loading_ = mode; }

void MapStore::configure(std::filesystem::path root) {
  clear();
  configured_ = std::move(root);
}

void MapStore::stop_worker() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  worker_ = std::jthread();
  const std::lock_guard lock(mutex_);
  jobs_.clear();
  done_.clear();
  busy_ = 0;
}

void MapStore::clear() {
  stop_worker();
  names_.clear();
  maps_.clear();
  read_.clear();
  asked_.clear();
  synced_ = 0;
  prefetch_serial_ = 0; // the pack's last prefetch is queued again
  ++generation_;
  resolved_ = false;
  enabled_ = false;
  root_.clear();
}

const std::filesystem::path& MapStore::root() {
  if (!resolved_) {
    resolved_ = true;
    root_ = configured_;
    if (root_.empty() && resources::HdPack::instance().enabled()) {
      root_ = resources::HdPack::instance().root() / "_maps";
    }
    std::error_code ec;
    enabled_ = !root_.empty() && std::filesystem::is_directory(root_, ec);
  }
  return root_;
}

bool MapStore::enabled() {
  (void)root();
  return enabled_;
}

std::string MapStore::key_for(std::string_view id, bool emissive, int w,
                              int h) {
  std::string key(id);
  key += emissive ? "|e|" : "|n|";
  key += std::to_string(w);
  key += 'x';
  key += std::to_string(h);
  return key;
}

void MapStore::bind(const std::shared_ptr<const gfx::HdImage>& picture,
                    std::string asset_id) {
  if (!picture) {
    return;
  }
  const int w = picture->width;
  const int h = picture->height;
  names_[picture.get()] = Named{picture, asset_id};
  maps_.erase(picture.get());
  // Its picture is on its way to the screen: its maps go first.
  if (!blocking() && enabled()) {
    if (normals_wanted_) {
      queue(asset_id, false, w, h, /*urgent=*/true);
    }
    queue(asset_id, true, w, h, /*urgent=*/true); // in front of it
  }
}

void MapStore::queue(const std::string& id, bool emissive, int w, int h,
                     bool urgent) {
  std::string key = key_for(id, emissive, w, h);
  if (read_.contains(key)) {
    return;
  }
  if (!asked_.insert(key).second) {
    if (urgent) {
      // Already waiting: move it to the front, if the worker hasn't taken
      // it yet.
      const std::lock_guard lock(mutex_);
      const auto it = std::ranges::find(jobs_, key, &Job::key);
      if (it != jobs_.end() && it != jobs_.begin()) {
        Job job = std::move(*it);
        jobs_.erase(it);
        jobs_.push_front(std::move(job));
      }
    }
    return;
  }
  Job job{.key = std::move(key),
          .path = root() / id / (emissive ? "emissive.png" : "normal.png"),
          .id = id,
          .emissive = emissive,
          .w = w,
          .h = h,
          .feather = emissive ? emissive_feather_radius(gfx::hd_scale()) : 0};
  if (!worker_.joinable()) {
    try {
      worker_ =
          std::jthread([this](const std::stop_token& stop) { run(stop); });
    }
    catch (const std::system_error& e) {
      Logger::warn("HD maps: cannot read them in the background ({}); "
                   "reading them when first drawn",
                   e.what());
      loading_ = Loading::Blocking;
      asked_.erase(job.key);
      return;
    }
  }
  {
    const std::lock_guard lock(mutex_);
    if (urgent) {
      jobs_.push_front(std::move(job));
    }
    else {
      jobs_.push_back(std::move(job));
    }
  }
  wake_.notify_one();
}

void MapStore::queue_prefetched() {
  const auto request = resources::HdPack::instance().prefetch_request();
  if (request.serial == prefetch_serial_) {
    return;
  }
  prefetch_serial_ = request.serial;
  if (!request.entries || blocking()) {
    return;
  }
  // The pictures the pack decodes in the background, in its order and
  // within its budget: their maps follow them.
  const int k = gfx::hd_scale();
  std::size_t bytes = 0;
  for (const auto& e : *request.entries) {
    if (bytes >= request.budget_bytes) {
      break;
    }
    std::string_view rel = e.relative;
    if (!rel.ends_with(".png")) {
      continue;
    }
    rel.remove_suffix(4);
    const int w = e.engine_size.w * k;
    const int h = e.engine_size.h * k;
    bytes += static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
             sizeof(uint32_t);
    const std::string id(rel);
    queue(id, true, w, h, /*urgent=*/false);
    if (normals_wanted_) {
      queue(id, false, w, h, /*urgent=*/false);
    }
  }
}

void MapStore::run(const std::stop_token& stop) {
  std::unique_lock lock(mutex_);
  while (true) {
    if (!wake_.wait(lock, stop, [this] { return !jobs_.empty(); }) ||
        stop.stop_requested()) {
      return; // stopped: what is left is dropped (see stop_worker)
    }
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    ++busy_;
    lock.unlock();
    Done done = read(job);
    lock.lock();
    done_.push_back(std::move(done));
    --busy_;
    if (jobs_.empty() && busy_ == 0) {
      idle_.notify_all();
    }
  }
}

std::size_t MapStore::pending() const {
  const std::lock_guard lock(mutex_);
  return jobs_.size() + busy_;
}

void MapStore::wait_for_loads() {
  if (worker_.joinable()) {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && busy_ == 0; });
  }
  collect();
}

void MapStore::collect() {
  std::vector<Done> done;
  {
    const std::lock_guard lock(mutex_);
    done.swap(done_);
  }
  bool any = false;
  for (Done& d : done) {
    if (!d.warning.empty()) {
      Logger::warn("{}", d.warning);
    }
    asked_.erase(d.key);
    any |= d.image != nullptr;
    read_[d.key] = std::move(d.image);
  }
  if (any) {
    ++generation_;
  }
}

void MapStore::sync() {
  if (!enabled()) {
    return;
  }
  collect();
  queue_prefetched();
  resources::HdPack& pack = resources::HdPack::instance();
  const auto used = pack.used();
  for (std::size_t i = synced_; i < used.size(); ++i) {
    std::string_view rel = used[i].relative;
    if (!rel.ends_with(".png")) {
      continue;
    }
    rel.remove_suffix(4);
    // A cache hit: the very picture the pack handed out.
    if (auto picture = pack.vga(rel, used[i].engine_size)) {
      bind(picture, std::string(rel));
    }
  }
  synced_ = used.size();
}

const MapStore::Named* MapStore::named(const gfx::HdImage* picture) {
  const auto it = names_.find(picture);
  if (it == names_.end()) {
    return nullptr;
  }
  // The address may have been reused by another picture since.
  const auto alive = it->second.picture.lock();
  if (alive.get() != picture) {
    maps_.erase(picture);
    names_.erase(it);
    return nullptr;
  }
  return &it->second;
}

namespace {

// One box pass along rows (dx = 1) or columns, edges repeated; `in` and
// `out` are premultiplied 0xRRGGBBAA texels.
void box_pass(const std::vector<uint32_t>& in, std::vector<uint32_t>& out,
              int w, int h, int radius, bool rows) {
  const int len = rows ? w : h;
  const int lines = rows ? h : w;
  const int n = 2 * radius + 1;
  std::vector<int> line(static_cast<std::size_t>(len));
  for (int l = 0; l < lines; ++l) {
    auto at = [&](int i) -> std::size_t {
      return rows ? static_cast<std::size_t>(l) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(i)
                  : static_cast<std::size_t>(i) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(l);
    };
    std::array<uint32_t, 4> result_shift{24, 16, 8, 0};
    std::vector<uint32_t> result(static_cast<std::size_t>(len), 0);
    for (const uint32_t shift : result_shift) {
      for (int i = 0; i < len; ++i) {
        line[static_cast<std::size_t>(i)] =
            static_cast<int>((in[at(i)] >> shift) & 0xFF);
      }
      auto value = [&](int i) {
        return line[static_cast<std::size_t>(std::clamp(i, 0, len - 1))];
      };
      int sum = 0;
      for (int i = -radius; i <= radius; ++i) {
        sum += value(i);
      }
      for (int i = 0; i < len; ++i) {
        const auto v = static_cast<uint32_t>((sum + n / 2) / n);
        result[static_cast<std::size_t>(i)] |= v << shift;
        sum += value(i + radius + 1) - value(i - radius);
      }
    }
    for (int i = 0; i < len; ++i) {
      out[at(i)] = result[static_cast<std::size_t>(i)];
    }
  }
}

} // namespace

void feather(gfx::HdImage& image, int radius) {
  if (radius <= 0 || image.width <= 0 || image.height <= 0) {
    return;
  }
  const int w = image.width;
  const int h = image.height;
  std::vector<uint32_t> a(image.pixels.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    const uint32_t t = image.pixels[i];
    const uint32_t al = gfx::alpha(t);
    auto pm = [al](uint32_t c) { return (c * al + 127) / 255; };
    a[i] = gfx::pack_rgba(pm(gfx::red(t)), pm(gfx::green(t)), pm(gfx::blue(t)),
                          al);
  }
  std::vector<uint32_t> b(a.size());
  for (int pass = 0; pass < 2; ++pass) {
    box_pass(a, b, w, h, radius, true);
    box_pass(b, a, w, h, radius, false);
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const uint32_t t = a[i];
    const uint32_t al = gfx::alpha(t);
    auto un = [al](uint32_t c) {
      return al == 0 ? 0u : std::min(255u, (c * 255 + al / 2) / al);
    };
    image.pixels[i] = gfx::pack_rgba(un(gfx::red(t)), un(gfx::green(t)),
                                     un(gfx::blue(t)), al);
  }
}

MapStore::Done MapStore::read(const Job& job) {
  Done d{.key = job.key, .image = nullptr, .warning = {}};
  const char* file = job.emissive ? "emissive.png" : "normal.png";
  try {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(job.path, ec)) {
      return d; // no such map: the usual case
    }
    std::string error;
    auto image = gfx::load_png(job.path, {}, &error);
    if (!image) {
      d.warning = "HD maps: " + error + "; ignoring it";
      return d;
    }
    const double want = static_cast<double>(job.w) / job.h;
    const double have = static_cast<double>(image->width) / image->height;
    if (std::abs(have / want - 1.0) > resources::HdPack::aspect_tolerance) {
      d.warning = "HD maps: " + job.id + "/" + file +
                  " has another shape than its picture; ignoring it";
      return d;
    }
    gfx::HdImage map = image->width != job.w || image->height != job.h
                           ? gfx::resample(*image, job.w, job.h)
                           : std::move(*image);
    feather(map, job.feather);
    d.image = std::make_shared<const gfx::HdImage>(std::move(map));
  }
  catch (const std::exception& e) {
    d.image.reset();
    d.warning =
        "HD maps: " + job.id + "/" + file + ": " + e.what() + "; ignoring it";
  }
  catch (...) {
    d.image.reset();
    d.warning = "HD maps: " + job.id + "/" + file + " failed; ignoring it";
  }
  return d;
}

MapStore::ImagePtr MapStore::lookup(const gfx::HdImage* picture,
                                    bool emissive) {
  if (!picture || names_.empty()) {
    return nullptr;
  }
  const Named* n = named(picture);
  if (!n) {
    return nullptr;
  }
  Maps& m = maps_[picture];
  bool& settled = emissive ? m.has_emissive : m.has_normal;
  ImagePtr& map = emissive ? m.emissive : m.normal;
  if (settled) {
    return map;
  }
  if (!enabled()) {
    settled = true;
    return nullptr;
  }
  std::string& key = emissive ? m.emissive_key : m.normal_key;
  if (key.empty()) {
    key = key_for(n->id, emissive, picture->width, picture->height);
  }
  if (const auto it = read_.find(key); it != read_.end()) {
    map = std::move(it->second);
    read_.erase(it);
    settled = true;
    return map;
  }
  if (blocking()) {
    const Done d = read(Job{
        .key = key,
        .path = root() / n->id / (emissive ? "emissive.png" : "normal.png"),
        .id = n->id,
        .emissive = emissive,
        .w = picture->width,
        .h = picture->height,
        .feather = emissive ? emissive_feather_radius(gfx::hd_scale()) : 0});
    if (!d.warning.empty()) {
      Logger::warn("{}", d.warning);
    }
    map = d.image;
    settled = true;
    return map;
  }
  // Not read yet: nothing this frame. (Queued by bind(); again if the
  // folder only appeared since.)
  const std::string id = n->id;
  queue(id, emissive, picture->width, picture->height, /*urgent=*/true);
  return nullptr;
}

std::shared_ptr<const gfx::HdImage>
MapStore::emissive(const gfx::HdImage* picture) {
  return lookup(picture, true);
}

std::shared_ptr<const gfx::HdImage>
MapStore::normal(const gfx::HdImage* picture) {
  return lookup(picture, false);
}

} // namespace fx
