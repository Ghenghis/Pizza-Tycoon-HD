/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/Size.h"
#include "engine/gfx/HdImage.h"

namespace resources {

// A folder of high-resolution PNG replacements for the original pictures.
//
// The layout mirrors the asset export (see AssetExporter.h):
//
//   <pack>/gfx/back1.png          replaces GFX/BACK1.VGA
//   <pack>/bob/moebel/0012.png    replaces frame 12 of BOB/MOEBEL.BOB
//
// Paths are lower case with forward slashes. A replacement may be any size
// as long as it keeps the original's aspect ratio; it is resized once, on
// first use, to exactly the engine size times gfx::hd_scale(). Anything
// missing from the pack keeps the original art.
class HdPack {
public:
  struct Stats {
    std::size_t loaded = 0;
    std::size_t missing = 0;
    std::size_t rejected = 0;
    double load_ms = 0; // reading, decoding and resizing pack files
    // Files that took over 10 ms, with their times (for the bench command).
    std::vector<std::pair<std::string, double>> slow;
  };

  static HdPack& instance();

  // An empty root (or hd_scale() of 1) turns the pack off.
  void configure(const std::filesystem::path& root);
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }

  // engine_size is the size of the Surface the original loads into (the
  // original picture doubled). Returns null when there is no usable
  // replacement.
  [[nodiscard]] std::shared_ptr<const gfx::HdImage> vga(std::string_view key,
                                                        Size engine_size);
  [[nodiscard]] std::shared_ptr<const gfx::HdImage>
  bob_frame(std::string_view key, std::size_t frame, Size engine_size);

  // "GFX\\Back1.VGA" -> "gfx/back1". Absolute paths keep only the file name
  // under "plegacy/", since they are the engine's own files.
  [[nodiscard]] static std::string normalize_key(std::string_view key);
  [[nodiscard]] static std::string vga_relative_path(std::string_view key);
  [[nodiscard]] static std::string bob_frame_relative_path(std::string_view key,
                                                           std::size_t frame);

  [[nodiscard]] Stats stats() const;

  /* Warm list: the pack files a session used, so the next session can decode
     them on a background thread while the intro plays, and a screen's HD
     art is ready when the screen first opens (no pause). */

  struct WarmEntry {
    std::string relative; // e.g. "gfx/bo.png"
    Size engine_size;
  };
  // Pack files loaded so far this session, in the order first used.
  [[nodiscard]] std::vector<WarmEntry> used() const;
  static bool save_warm_list(const std::filesystem::path& file,
                             const std::vector<WarmEntry>& entries);
  [[nodiscard]] static std::vector<WarmEntry>
  load_warm_list(const std::filesystem::path& file);
  // Decodes these on one background thread, stopping after budget_bytes of
  // decoded pictures. A load() of a file that is being decoded waits for it
  // instead of decoding it twice.
  void prefetch(std::vector<WarmEntry> entries, std::size_t budget_bytes);
  // Blocks until a running prefetch has finished (tests, benchmarks).
  void wait_for_prefetch();

  HdPack() = default;
  ~HdPack();
  HdPack(const HdPack&) = delete;
  HdPack& operator=(const HdPack&) = delete;

  // Largest allowed difference between the replacement's aspect ratio and
  // the original's, as a fraction.
  static constexpr double aspect_tolerance = 0.02;

private:
  using ImagePtr = std::shared_ptr<const gfx::HdImage>;
  struct Decoded {
    ImagePtr image;
    enum class Outcome { Loaded, Missing, Rejected } outcome;
    double ms;
    std::string warning;
  };
  // Reads, decodes and resizes one file. Touches no member but root (set
  // before any thread starts), so it runs on any thread.
  [[nodiscard]] Decoded decode(const std::string& relative,
                               Size engine_size) const;
  // Under mutex_: counts the result, caches it, notes the first use.
  void record(const std::string& cache_key, const WarmEntry& entry,
              const Decoded& d);
  ImagePtr load(const std::string& relative, Size engine_size);
  void stop_prefetch();

  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ImagePtr> cache_;
  std::unordered_map<std::string, std::shared_future<ImagePtr>> pending_;
  std::vector<WarmEntry> used_;
  Stats stats_;
  std::jthread prefetcher_;
};

} // namespace resources
