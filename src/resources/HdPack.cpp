/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resources/HdPack.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "engine/FormatCompat.h"
#include "engine/Logger.h"
#include "engine/gfx/Png.h"

namespace resources {

namespace {

std::string utf8(const std::filesystem::path& p) {
  const auto u8 = p.generic_u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

} // namespace

HdPack& HdPack::instance() {
  static HdPack pack;
  return pack;
}

HdPack::~HdPack() { stop_prefetch(); }

void HdPack::stop_prefetch() {
  if (prefetcher_.joinable()) {
    prefetcher_.request_stop();
    prefetcher_.join();
  }
}

void HdPack::wait_for_prefetch() {
  if (prefetcher_.joinable()) {
    prefetcher_.join();
  }
}

HdPack::Stats HdPack::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

std::vector<HdPack::WarmEntry> HdPack::used() const {
  const std::lock_guard lock(mutex_);
  return used_;
}

void HdPack::configure(const std::filesystem::path& root) {
  stop_prefetch();
  const std::lock_guard lock(mutex_);
  cache_.clear();
  pending_.clear();
  used_.clear();
  stats_ = {};
  root_.clear();
  if (root.empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    Logger::warn("HD pack folder {} does not exist; using the original art",
                 utf8(root));
    return;
  }
  root_ = root;
  if (gfx::hd_scale() <= 1) {
    Logger::warn("HD pack {} is set but hd_scale is 1; set hd_scale to 2-8 "
                 "to use it",
                 utf8(root));
    return;
  }
  Logger::info("HD pack: {} at {}x", utf8(root_), gfx::hd_scale());
}

bool HdPack::enabled() const noexcept {
  return !root_.empty() && gfx::hd_scale() > 1;
}

std::string HdPack::normalize_key(std::string_view key) {
  std::string s(key);
  std::replace(s.begin(), s.end(), '\\', '/');
  const std::filesystem::path p(s);
  if (p.is_absolute()) {
    s = "plegacy/" + p.filename().generic_string();
  }
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const char* ext : {".vga", ".bob"}) {
    const std::size_t n = std::strlen(ext);
    if (s.size() > n && s.compare(s.size() - n, n, ext) == 0) {
      s.resize(s.size() - n);
    }
  }
  while (s.starts_with("./")) {
    s.erase(0, 2);
  }
  return s;
}

std::string HdPack::vga_relative_path(std::string_view key) {
  return normalize_key(key) + ".png";
}

std::string HdPack::bob_frame_relative_path(std::string_view key,
                                            std::size_t frame) {
  char name[16];
  std::snprintf(name, sizeof(name), "%04zu.png", frame);
  return normalize_key(key) + "/" + name;
}

std::shared_ptr<const gfx::HdImage> HdPack::vga(std::string_view key,
                                                Size engine_size) {
  if (!enabled()) {
    return nullptr;
  }
  return load(vga_relative_path(key), engine_size);
}

std::shared_ptr<const gfx::HdImage>
HdPack::bob_frame(std::string_view key, std::size_t frame, Size engine_size) {
  if (!enabled()) {
    return nullptr;
  }
  return load(bob_frame_relative_path(key, frame), engine_size);
}

namespace {

std::string cache_key_for(const std::string& relative, int w, int h) {
  return relative + "@" + std::to_string(w) + "x" + std::to_string(h);
}

} // namespace

HdPack::Decoded HdPack::decode(const std::string& relative,
                               Size engine_size) const {
  const auto started = std::chrono::steady_clock::now();
  const int k = gfx::hd_scale();
  const int target_w = engine_size.w * k;
  const int target_h = engine_size.h * k;
  Decoded d{nullptr, Decoded::Outcome::Missing, 0.0, {}};
  const std::filesystem::path file = root_ / relative;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    d.outcome = Decoded::Outcome::Missing;
  }
  else if (auto image = gfx::load_png(file)) {
    const double want =
        static_cast<double>(engine_size.w) / static_cast<double>(engine_size.h);
    const double have =
        static_cast<double>(image->width) / static_cast<double>(image->height);
    if (std::abs(have / want - 1.0) > aspect_tolerance) {
      d.outcome = Decoded::Outcome::Rejected;
      d.warning = compat::format(
          "HD pack: {} is {}x{} but the original is {}x{} (aspect ratio "
          "differs); ignoring it",
          relative, image->width, image->height, engine_size.w / 2,
          engine_size.h / 2);
    }
    else if (image->width == target_w && image->height == target_h) {
      d.image = std::make_shared<const gfx::HdImage>(std::move(*image));
      d.outcome = Decoded::Outcome::Loaded;
    }
    else {
      d.image = std::make_shared<const gfx::HdImage>(
          gfx::resample(*image, target_w, target_h));
      d.outcome = Decoded::Outcome::Loaded;
    }
  }
  else {
    d.outcome = Decoded::Outcome::Rejected;
  }
  d.ms = std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
             .count();
  return d;
}

void HdPack::record(const std::string& cache_key, const WarmEntry& entry,
                    const Decoded& d) {
  switch (d.outcome) {
  case Decoded::Outcome::Loaded:
    ++stats_.loaded;
    break;
  case Decoded::Outcome::Missing:
    ++stats_.missing;
    break;
  case Decoded::Outcome::Rejected:
    ++stats_.rejected;
    break;
  }
  stats_.load_ms += d.ms;
  if (d.ms > 10.0 && d.image) {
    stats_.slow.emplace_back(entry.relative, d.ms);
  }
  cache_.emplace(cache_key, d.image);
  if (d.image) {
    used_.push_back(entry);
  }
}

std::shared_ptr<const gfx::HdImage> HdPack::load(const std::string& relative,
                                                 Size engine_size) {
  const int k = gfx::hd_scale();
  const std::string key =
      cache_key_for(relative, engine_size.w * k, engine_size.h * k);
  std::shared_future<ImagePtr> in_flight;
  std::promise<ImagePtr> mine;
  {
    const std::lock_guard lock(mutex_);
    if (auto it = cache_.find(key); it != cache_.end()) {
      return it->second;
    }
    if (auto it = pending_.find(key); it != pending_.end()) {
      in_flight = it->second;
    }
    else {
      // Claim it, so a prefetch reaching the same file waits for us.
      pending_.emplace(key, mine.get_future().share());
    }
  }
  if (in_flight.valid()) {
    return in_flight.get(); // the prefetcher is decoding it right now
  }
  const Decoded d = decode(relative, engine_size);
  if (!d.warning.empty()) {
    Logger::warn("{}", d.warning);
  }
  ImagePtr result;
  {
    const std::lock_guard lock(mutex_);
    record(key, WarmEntry{relative, engine_size}, d);
    pending_.erase(key);
    result = cache_.at(key);
  }
  mine.set_value(result);
  return result;
}

void HdPack::prefetch(std::vector<WarmEntry> entries,
                      std::size_t budget_bytes) {
  stop_prefetch();
  if (!enabled() || entries.empty()) {
    return;
  }
  prefetcher_ = std::jthread([this, entries = std::move(entries),
                              budget_bytes](const std::stop_token& stop) {
    std::size_t bytes = 0;
    for (const WarmEntry& e : entries) {
      if (stop.stop_requested() || bytes >= budget_bytes) {
        return;
      }
      const int k = gfx::hd_scale();
      const std::string key =
          cache_key_for(e.relative, e.engine_size.w * k, e.engine_size.h * k);
      std::promise<ImagePtr> promise;
      {
        const std::lock_guard lock(mutex_);
        if (cache_.contains(key) || pending_.contains(key)) {
          continue;
        }
        pending_.emplace(key, promise.get_future().share());
      }
      const Decoded d = decode(e.relative, e.engine_size);
      {
        const std::lock_guard lock(mutex_);
        record(key, e, d);
        pending_.erase(key);
      }
      promise.set_value(d.image);
      if (d.image) {
        bytes += d.image->pixels.size() * sizeof(uint32_t);
      }
    }
  });
}

bool HdPack::save_warm_list(const std::filesystem::path& file,
                            const std::vector<WarmEntry>& entries) {
  std::ofstream out(file, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "# Pizza Tycoon HD: pack files used last session, decoded early\n";
  for (const WarmEntry& e : entries) {
    out << e.engine_size.w << ' ' << e.engine_size.h << ' ' << e.relative
        << '\n';
  }
  return static_cast<bool>(out);
}

std::vector<HdPack::WarmEntry>
HdPack::load_warm_list(const std::filesystem::path& file) {
  std::vector<WarmEntry> entries;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream fields(line);
    WarmEntry e;
    if (fields >> e.engine_size.w >> e.engine_size.h >> e.relative &&
        e.engine_size.w > 0 && e.engine_size.h > 0 &&
        e.relative.find("..") == std::string::npos &&
        !std::filesystem::path(e.relative).is_absolute()) {
      entries.push_back(std::move(e));
    }
  }
  return entries;
}

} // namespace resources
