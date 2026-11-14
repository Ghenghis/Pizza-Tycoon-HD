/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/gfx/HdImage.h"

namespace gfx {

// Limits a PNG must meet, checked from its header before anything is
// decoded, so a broken or hostile file cannot make the game allocate
// gigabytes.
struct PngLimits {
  static constexpr std::uintmax_t default_max_file_bytes = std::uintmax_t{256}
                                                           << 20; // 256 MiB
  static constexpr int default_max_side = 16384;
  // 128 Mi texels: 512 MiB once decoded to RGBA.
  static constexpr std::uint64_t default_max_pixels = std::uint64_t{128} << 20;

  std::uintmax_t max_file_bytes = default_max_file_bytes; // encoded size
  int max_side = default_max_side;                        // texels per side
  std::uint64_t max_pixels = default_max_pixels;          // width x height
};

// A PNG's size, from its signature and IHDR chunk.
struct PngInfo {
  int width = 0;
  int height = 0;
};

// The signature and the IHDR chunk: the first 33 bytes of every PNG.
inline constexpr std::size_t png_header_bytes = 33;

// Reads the size from the start of a PNG file's bytes. Returns nullopt, with
// the reason in *error when given, when they do not start like a valid PNG.
[[nodiscard]] std::optional<PngInfo>
parse_png_header(std::span<const std::uint8_t> bytes,
                 std::string* error = nullptr);

// Empty when a PNG of this size and file length is within the limits;
// otherwise which limit it breaks.
[[nodiscard]] std::string check_png_limits(const PngInfo& info,
                                           std::uintmax_t file_bytes,
                                           const PngLimits& limits = {});

// Reads only a PNG file's header and checks it against the limits. Returns
// nullopt when the file cannot be read, is not a PNG or breaks a limit; the
// reason goes to *error when given, and to the log otherwise.
[[nodiscard]] std::optional<PngInfo>
read_png_info(const std::filesystem::path& path, const PngLimits& limits = {},
              std::string* error = nullptr);

// Reads a PNG into straight-alpha RGBA texels, checking its header against
// the limits before reading the rest. Returns null when the file cannot be
// read, is not a valid PNG or breaks a limit; the reason goes to *error when
// given, and to the log otherwise. Pass error from threads other than the
// game's, since the log is not thread-safe.
[[nodiscard]] std::unique_ptr<HdImage>
load_png(const std::filesystem::path& path, const PngLimits& limits = {},
         std::string* error = nullptr);

// Writes straight-alpha RGBA texels as a PNG, creating parent folders.
// Returns false (and logs a warning) on failure.
[[nodiscard]] bool save_png(const HdImage& image,
                            const std::filesystem::path& path);

} // namespace gfx
