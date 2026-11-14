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
#include <exception>
#include <fstream>
#include <optional>
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
  log_prefetch_warnings();
}

std::vector<std::shared_ptr<const gfx::HdImage>> HdPack::take_prefetched() {
  log_prefetch_warnings();
  const std::lock_guard lock(mutex_);
  std::vector<ImagePtr> out;
  out.swap(prefetched_);
  return out;
}

void HdPack::note_prefetch_warning(std::string_view text,
                                   std::string_view detail) noexcept {
  try {
    std::string message(text);
    message += detail;
    const std::lock_guard lock(mutex_);
    prefetch_warnings_.push_back(std::move(message));
  }
  catch (...) {
    // Out of memory: lose the message, not the program.
  }
}

void HdPack::log_prefetch_warnings() {
  std::vector<std::string> warnings;
  {
    const std::lock_guard lock(mutex_);
    warnings.swap(prefetch_warnings_);
  }
  for (const std::string& w : warnings) {
    Logger::warn("{}", w);
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
  log_prefetch_warnings();
  const std::lock_guard lock(mutex_);
  cache_.clear();
  pending_.clear();
  used_.clear();
  prefetched_.clear();
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

// Whether a w x h replacement keeps the original's aspect ratio.
bool same_shape(int w, int h, Size engine_size) {
  const double want =
      static_cast<double>(engine_size.w) / static_cast<double>(engine_size.h);
  const double have = static_cast<double>(w) / static_cast<double>(h);
  return std::abs(have / want - 1.0) <= HdPack::aspect_tolerance;
}

} // namespace

HdPack::Decoded HdPack::decode(const std::string& relative,
                               Size engine_size) const {
  const auto started = std::chrono::steady_clock::now();
  Decoded d{nullptr, Decoded::Outcome::Missing, 0.0, {}};
  try {
    const int k = gfx::hd_scale();
    const int target_w = engine_size.w * k;
    const int target_h = engine_size.h * k;
    const std::filesystem::path file = root_ / relative;
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec)) {
      d.outcome = Decoded::Outcome::Rejected; // until it loads
      std::string error;
      // The header settles the size limits and the shape before anything is
      // decoded.
      const auto info = gfx::read_png_info(file, {}, &error);
      if (!info) {
        d.warning =
            compat::format("HD pack: {}; using the original art", error);
      }
      else if (!same_shape(info->width, info->height, engine_size)) {
        d.warning = compat::format(
            "HD pack: {} is {}x{} but the original is {}x{} (aspect ratio "
            "differs); ignoring it",
            relative, info->width, info->height, engine_size.w / 2,
            engine_size.h / 2);
      }
      else if (auto image = gfx::load_png(file, {}, &error); !image) {
        d.warning =
            compat::format("HD pack: {}; using the original art", error);
      }
      else if (image->width != info->width || image->height != info->height) {
        d.warning = compat::format(
            "HD pack: {} changed while it was loading; using the original art",
            relative);
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
  }
  catch (const std::exception& e) {
    // Out of memory, or a library failing: keep the original art.
    d.image.reset();
    d.outcome = Decoded::Outcome::Rejected;
    d.warning =
        compat::format("HD pack: cannot load {} ({}); using the original art",
                       relative, e.what());
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

// A file this thread has claimed in pending_, so a load() of the same file
// waits for this decode instead of starting another. The claim is resolved
// exactly once: finish() records the result and hands it to the waiters; if
// an exception skips finish(), the destructor drops the claim and resolves
// the waiters with null, so they keep the original art and nobody waits
// forever. Never destroy one while holding mutex_.
class HdPack::Claim {
public:
  // Call under mutex_.
  Claim(HdPack& pack, std::string key) : pack_(pack), key_(std::move(key)) {
    pack_.pending_.emplace(key_, promise_.get_future().share());
  }
  ~Claim() {
    if (resolved_) {
      return;
    }
    {
      const std::lock_guard lock(pack_.mutex_);
      pack_.pending_.erase(key_);
    }
    promise_.set_value(nullptr);
  }
  Claim(const Claim&) = delete;
  Claim& operator=(const Claim&) = delete;

  ImagePtr finish(const Decoded& d, const WarmEntry& entry, bool prefetched) {
    {
      const std::lock_guard lock(pack_.mutex_);
      pack_.record(key_, entry, d);
      if (prefetched && d.image) {
        pack_.prefetched_.push_back(d.image);
      }
      pack_.pending_.erase(key_);
    }
    resolved_ = true;
    promise_.set_value(d.image);
    return d.image;
  }

private:
  HdPack& pack_;
  std::string key_;
  std::promise<ImagePtr> promise_;
  bool resolved_ = false;
};

std::shared_ptr<const gfx::HdImage> HdPack::load(const std::string& relative,
                                                 Size engine_size) {
  const int k = gfx::hd_scale();
  const std::string key =
      cache_key_for(relative, engine_size.w * k, engine_size.h * k);
  std::shared_future<ImagePtr> in_flight;
  std::optional<Claim> claim;
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
      claim.emplace(*this, key);
    }
  }
  if (in_flight.valid()) {
    return in_flight.get(); // the prefetcher is decoding it right now
  }
  const Decoded d = decode(relative, engine_size);
  if (!d.warning.empty()) {
    Logger::warn("{}", d.warning);
  }
  return claim->finish(d, WarmEntry{relative, engine_size},
                       /*prefetched=*/false);
}

void HdPack::run_prefetch(const std::vector<WarmEntry>& entries,
                          std::size_t budget_bytes,
                          const std::stop_token& stop) {
  std::size_t bytes = 0;
  for (const WarmEntry& e : entries) {
    if (stop.stop_requested() || bytes >= budget_bytes) {
      return;
    }
    const int k = gfx::hd_scale();
    const std::string key =
        cache_key_for(e.relative, e.engine_size.w * k, e.engine_size.h * k);
    std::optional<Claim> claim;
    {
      const std::lock_guard lock(mutex_);
      if (cache_.contains(key) || pending_.contains(key)) {
        continue;
      }
      claim.emplace(*this, key);
    }
    const Decoded d = decode(e.relative, e.engine_size);
    if (!d.warning.empty()) {
      note_prefetch_warning(d.warning);
    }
    if (const ImagePtr image = claim->finish(d, e, /*prefetched=*/true)) {
      bytes += image->pixels.size() * sizeof(uint32_t);
    }
  }
}

void HdPack::prefetch(std::vector<WarmEntry> entries,
                      std::size_t budget_bytes) {
  stop_prefetch();
  log_prefetch_warnings();
  if (!enabled() || entries.empty()) {
    return;
  }
  try {
    prefetcher_ = std::jthread([this, entries = std::move(entries),
                                budget_bytes](const std::stop_token& stop) {
      // An exception leaving a thread ends the program, so stop here and
      // log it instead. Claims already resolved every file taken so far.
      try {
        run_prefetch(entries, budget_bytes, stop);
      }
      catch (const std::exception& e) {
        note_prefetch_warning("HD pack: background loading stopped: ",
                              e.what());
      }
      catch (...) {
        note_prefetch_warning("HD pack: background loading stopped");
      }
    });
  }
  catch (const std::system_error& e) {
    Logger::warn("HD pack: cannot start background loading ({}); pictures "
                 "load when first used",
                 e.what());
  }
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
