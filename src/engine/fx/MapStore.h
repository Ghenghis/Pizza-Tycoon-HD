/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "engine/gfx/HdImage.h"

// The optional per-picture maps of the HD pack, made offline by
// tools/hdpack/maps.py (docs/studio/notes/hdpack-maps-layout.md):
//
//   <maps>/<asset id>/normal.png     tangent-space normals, RGB 8-bit:
//                                    R = +X right, G = +Y up, B = +Z out
//   <maps>/<asset id>/emissive.png   RGBA 8-bit, straight alpha: the light
//                                    the picture gives off by itself (lit
//                                    windows, neon, flames), alpha = how
//                                    much; transparent = none
//
// <maps> is the pack's "_maps" folder unless configured otherwise; the
// asset id is the pack picture's path without ".png" (gfx/back1,
// bob/moebel/0012). A map may be any size with the picture's aspect ratio;
// it is resized to the picture. Without a map nothing changes: every lookup
// is null and costs a hash lookup.
//
// Maps are read off the game thread (see MapStore::Loading): queued when
// their picture is queued by the pack's prefetch or first loaded, read,
// resized and feathered by one worker thread, and taken in by sync().
namespace fx {

// Softens an emissive map's edges: two box blurs of `radius` texels (close
// to a tent of 4 * radius + 1) in premultiplied space, so a glow never
// bleeds the colour of transparent texels. Radius 0 leaves it as it is.
// Masks are cut on the original's pixel grid; at hd_scale 6 and more that
// grid shows as blocky glow edges, so maps are feathered on load by
// emissive_feather_radius(hd_scale()).
void feather(gfx::HdImage& image, int radius);
// k / 2 texels from hd_scale 6 up (half an original pixel at engine scale
// 2), none below.
[[nodiscard]] constexpr int emissive_feather_radius(int k) {
  return k >= 6 ? k / 2 : 0;
}

class MapStore {
public:
  static MapStore& instance();

  // How maps are read. Background (the default): on a worker thread, which
  // also resizes and feathers them; a lookup of a map not read yet is null
  // (the picture draws without it that frame) and the renderer draws a map
  // only once stream() has put it on the GPU, within the per-frame upload
  // budget. Blocking: read on first lookup and uploaded when first drawn,
  // as before (captures, goldens, the "before" of the benchmark).
  enum class Loading : uint8_t { Background, Blocking };
  void loading(Loading mode);
  [[nodiscard]] Loading loading() const noexcept { return loading_; }
  [[nodiscard]] bool blocking() const noexcept {
    return loading_ == Loading::Blocking;
  }

  // Blocking loading for a scope (a capture: the same frame every time,
  // maps and all), then back to what it was.
  class ScopedBlocking {
  public:
    ScopedBlocking() : saved_(instance().loading()) {
      instance().loading(Loading::Blocking);
    }
    ~ScopedBlocking() { instance().loading(saved_); }
    ScopedBlocking(const ScopedBlocking&) = delete;
    ScopedBlocking& operator=(const ScopedBlocking&) = delete;

  private:
    Loading saved_;
  };

  // Whether normal maps are drawn at all (the renderer's relief pass, GPU
  // drivers only; it says so each frame). Without it they are not read
  // ahead, only when looked up. Emissive maps are always read ahead.
  void want_normals(bool on) noexcept { normals_wanted_ = on; }

  // An empty root falls back to <HD pack root>/_maps.
  void configure(std::filesystem::path root);
  [[nodiscard]] const std::filesystem::path& root();
  // True when there is a maps folder at all.
  [[nodiscard]] bool enabled();

  // The picture's maps, the picture's size, or null (none, or not read
  // yet).
  [[nodiscard]] std::shared_ptr<const gfx::HdImage>
  emissive(const gfx::HdImage* picture);
  [[nodiscard]] std::shared_ptr<const gfx::HdImage>
  normal(const gfx::HdImage* picture);

  // Names a picture by its asset id, and queues its maps. Pictures the HD
  // pack loaded are found by themselves (see sync()); others (tests,
  // made-up pictures) can be named here.
  void bind(const std::shared_ptr<const gfx::HdImage>& picture,
            std::string asset_id);
  // Call once per frame (cheap when nothing changed): learns the asset ids
  // of the pictures the HD pack loaded since the last call, queues the maps
  // of the pictures the pack's prefetch queued, and takes the maps the
  // worker has finished.
  void sync();
  // Grows by one each time sync() or wait_for_loads() takes in a map, so the
  // renderer knows to draw its glow and relief again.
  [[nodiscard]] std::size_t generation() const noexcept { return generation_; }
  // Maps queued or being read (tests, benchmarks).
  [[nodiscard]] std::size_t pending() const;
  // Blocks until the worker has read every queued map, then takes them in
  // (tests, captures, benchmarks).
  void wait_for_loads();
  // Forgets everything (tests; a new pack); maps still queued are dropped.
  void clear();

  MapStore() = default;
  ~MapStore();
  MapStore(const MapStore&) = delete;
  MapStore& operator=(const MapStore&) = delete;

private:
  using ImagePtr = std::shared_ptr<const gfx::HdImage>;
  struct Named {
    std::weak_ptr<const gfx::HdImage> picture;
    std::string id;
  };
  struct Maps {
    bool has_emissive = false; // looked up and settled (map or none)
    bool has_normal = false;
    ImagePtr emissive;
    ImagePtr normal;
    std::string emissive_key; // see key_for, made on first lookup
    std::string normal_key;
  };
  // One map file to read for a picture of w x h.
  struct Job {
    std::string key;
    std::filesystem::path path;
    std::string id;
    bool emissive = false;
    int w = 0;
    int h = 0;
    int feather = 0;
  };
  struct Done {
    std::string key;
    ImagePtr image;
    std::string warning;
  };
  [[nodiscard]] static std::string key_for(std::string_view id, bool emissive,
                                           int w, int h);
  // Reads, resizes and feathers one map; any thread (touches no member).
  [[nodiscard]] static Done read(const Job& job);
  const Named* named(const gfx::HdImage* picture);
  ImagePtr lookup(const gfx::HdImage* picture, bool emissive);
  // Game thread: queues the map unless it is read, queued or being read.
  // An urgent map goes to the front (its picture is being drawn).
  void queue(const std::string& id, bool emissive, int w, int h, bool urgent);
  void queue_prefetched();
  // Game thread: takes in what the worker finished; logs its warnings.
  void collect();
  void run(const std::stop_token& stop);
  void stop_worker();

  Loading loading_ = Loading::Background;
  bool normals_wanted_ = false;
  std::filesystem::path configured_;
  std::filesystem::path root_;
  bool resolved_ = false;
  bool enabled_ = false;
  std::size_t synced_ = 0;
  std::size_t prefetch_serial_ = 0;
  std::size_t generation_ = 0;
  std::unordered_map<const gfx::HdImage*, Named> names_;
  std::unordered_map<const gfx::HdImage*, Maps> maps_;
  // Game thread: maps read and not yet given to a picture, by key (null:
  // no such map), and the keys queued or being read.
  std::unordered_map<std::string, ImagePtr> read_;
  std::unordered_set<std::string> asked_;

  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::condition_variable_any idle_;
  std::deque<Job> jobs_;   // under mutex_
  std::vector<Done> done_; // under mutex_
  std::size_t busy_ = 0;   // under mutex_: jobs taken, not yet in done_
  std::jthread worker_;
};

} // namespace fx
