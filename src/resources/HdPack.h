/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  // Largest allowed difference between the replacement's aspect ratio and
  // the original's, as a fraction.
  static constexpr double aspect_tolerance = 0.02;

private:
  std::shared_ptr<const gfx::HdImage> load(const std::string& relative,
                                           Size engine_size);

  std::filesystem::path root_;
  std::unordered_map<std::string, std::shared_ptr<const gfx::HdImage>> cache_;
  Stats stats_;
};

} // namespace resources
