/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "engine/gfx/HdImage.h"
#include "engine/gfx/HdList.h"

// The caches behind the views' procedural HD pictures (CityMapHd,
// MediumMapHd, WaterCycleHd). Pictures are built once per distinct input and
// shared; workers (BackgroundJob) add to them too, so they are locked.
namespace views::hd {

// FNV-1a over the bytes of the inputs, for the cache keys.
class KeyHash {
public:
  void add(const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h_ = (h_ ^ p[i]) * 0x100000001b3ULL;
    }
  }
  template <typename T> void add(const T& v) { add(&v, sizeof v); }
  [[nodiscard]] uint64_t value() const { return h_; }

private:
  uint64_t h_ = 0xcbf29ce484222325ULL;
};

// A small cache of pictures by key, dropping the oldest past a byte budget.
class PictureCache {
public:
  explicit PictureCache(std::size_t budget_bytes) : budget_(budget_bytes) {}

  std::shared_ptr<const gfx::HdImage> find(uint64_t key) {
    std::lock_guard lock(mutex_);
    for (const auto& [k, img] : entries_) {
      if (k == key) {
        return img;
      }
    }
    return nullptr;
  }

  // Adds `img` under `key` and returns it, or the picture already there
  // when another thread got there first (so everyone shares one).
  std::shared_ptr<const gfx::HdImage>
  add(uint64_t key, std::shared_ptr<const gfx::HdImage> img) {
    std::lock_guard lock(mutex_);
    for (const auto& [k, there] : entries_) {
      if (k == key) {
        return there;
      }
    }
    bytes_ += bytes_of(*img);
    entries_.emplace_back(key, img);
    while (bytes_ > budget_ && entries_.size() > 1) {
      bytes_ -= bytes_of(*entries_.front().second);
      entries_.erase(entries_.begin());
    }
    return img;
  }

private:
  static std::size_t bytes_of(const gfx::HdImage& img) {
    return img.pixels.size() * sizeof(uint32_t);
  }

  std::mutex mutex_;
  std::vector<std::pair<uint64_t, std::shared_ptr<const gfx::HdImage>>>
      entries_;
  std::size_t bytes_ = 0;
  std::size_t budget_;
};

// The city map pictures: far maps (~11 MB each at 4K), the medium zoom's
// tile sheet (~2.4 MB) and the water frames (a few MB). A few cities and
// seasons in one session.
inline PictureCache& map_picture_cache() {
  static PictureCache cache(std::size_t{128} << 20);
  return cache;
}

// The district and control hatching (small, many colours).
inline PictureCache& hatch_picture_cache() {
  static PictureCache cache(std::size_t{64} << 20);
  return cache;
}

// Fills img's opacity cache (gfx::opaque_block) for scale k. Its first use
// fills it lazily, which is a write: a picture that a worker built must have
// it filled before another thread can see the picture.
inline void prefill_opacity(const gfx::HdImage& img, int k) {
  if (k > 1 && img.width >= k && img.height >= k) {
    (void)gfx::opaque_block(img, 0, 0, k);
  }
}

// A key for a picture's content that is cheap enough to take on the game
// thread: its address and size, plus a sparse sample of its texels (the
// address alone could come back for another picture after this one is
// freed).
inline void add_picture_identity(KeyHash& key, const gfx::HdImage& img) {
  key.add(&img);
  key.add(img.width);
  key.add(img.height);
  // ~4000 texels, spread over the whole picture.
  const std::size_t n = img.pixels.size();
  const std::size_t step = std::max<std::size_t>(1, n / 4093);
  for (std::size_t i = 0; i < n; i += step) {
    key.add(img.pixels[i]);
  }
}

} // namespace views::hd
