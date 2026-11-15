/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/PackWarmup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/util/FileUtils.h"

namespace fx {

namespace {

std::string lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// The original's width and height from its VGA header (two little-endian
// 16-bit words), or nothing.
std::optional<Size> vga_size(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::array<unsigned char, 4> h{};
  if (!in.read(reinterpret_cast<char*>(h.data()), h.size())) {
    return std::nullopt;
  }
  const int w = h[0] | (h[1] << 8);
  const int ht = h[2] | (h[3] << 8);
  // VgaLoader's limits.
  if (w <= 0 || ht <= 0 || w > 320 || ht > 320) {
    return std::nullopt;
  }
  return Size{w, ht};
}

} // namespace

std::vector<WarmEntry>
scan_pack_pictures(const std::filesystem::path& pack_root,
                   const std::filesystem::path& game_root) {
  std::vector<WarmEntry> out;
  std::error_code ec;
  const std::filesystem::path gfx = pack_root / "gfx";
  if (game_root.empty() || !std::filesystem::is_directory(gfx, ec)) {
    return out;
  }
  for (const auto& entry : std::filesystem::directory_iterator(gfx, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = lower(entry.path().filename().string());
    if (!name.ends_with(".png")) {
      continue;
    }
    const std::string stem = name.substr(0, name.size() - 4);
    const auto original = fileutils::case_insensitive_lookup(
        game_root, std::filesystem::path("gfx") / (stem + ".vga"));
    if (!original) {
      continue;
    }
    const auto size = vga_size(*original);
    const auto info = gfx::read_png_info(entry.path(), {}, nullptr);
    if (!size || !info) {
      continue;
    }
    const double want =
        static_cast<double>(size->w) / static_cast<double>(size->h);
    const double have =
        static_cast<double>(info->width) / static_cast<double>(info->height);
    if (std::abs(have / want - 1.0) > resources::HdPack::aspect_tolerance) {
      continue;
    }
    out.push_back({"gfx/" + name, Size{size->w * gfx::asset_pixel_scale,
                                       size->h * gfx::asset_pixel_scale}});
  }
  std::ranges::sort(out, [](const WarmEntry& a, const WarmEntry& b) {
    const long pa = static_cast<long>(a.engine_size.w) * a.engine_size.h;
    const long pb = static_cast<long>(b.engine_size.w) * b.engine_size.h;
    return pa != pb ? pa > pb : a.relative < b.relative;
  });
  return out;
}

std::vector<WarmEntry> session_warm_list(const std::filesystem::path& user_list,
                                         const std::filesystem::path& pack_root,
                                         const std::filesystem::path& game_root,
                                         int k, std::size_t scan_budget) {
  std::vector<WarmEntry> out;
  std::unordered_set<std::string> seen;
  auto add = [&](std::vector<WarmEntry> entries) {
    for (WarmEntry& e : entries) {
      const std::string key = e.relative + "@" +
                              std::to_string(e.engine_size.w) + "x" +
                              std::to_string(e.engine_size.h);
      if (seen.insert(key).second) {
        out.push_back(std::move(e));
      }
    }
  };
  add(resources::HdPack::load_warm_list(user_list));
  if (!pack_root.empty()) {
    add(resources::HdPack::load_warm_list(pack_root / "hd_warm.txt"));
    std::vector<WarmEntry> scanned = scan_pack_pictures(pack_root, game_root);
    std::vector<WarmEntry> kept;
    std::size_t bytes = 0;
    const auto kk = static_cast<std::size_t>(std::max(1, k));
    for (WarmEntry& e : scanned) {
      const std::size_t b = static_cast<std::size_t>(e.engine_size.w) * kk *
                            static_cast<std::size_t>(e.engine_size.h) * kk * 4;
      if (bytes + b > scan_budget) {
        break;
      }
      bytes += b;
      kept.push_back(std::move(e));
    }
    add(std::move(kept));
  }
  return out;
}

} // namespace fx
