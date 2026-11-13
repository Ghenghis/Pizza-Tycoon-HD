/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resources/HdPack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

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

void HdPack::configure(const std::filesystem::path& root) {
  cache_.clear();
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

std::shared_ptr<const gfx::HdImage> HdPack::load(const std::string& relative,
                                                 Size engine_size) {
  const int k = gfx::hd_scale();
  const int target_w = engine_size.w * k;
  const int target_h = engine_size.h * k;
  const std::string cache_key = relative + "@" + std::to_string(target_w) +
                                "x" + std::to_string(target_h);
  if (auto it = cache_.find(cache_key); it != cache_.end()) {
    return it->second;
  }

  std::shared_ptr<const gfx::HdImage> result;
  const std::filesystem::path file = root_ / relative;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) {
    ++stats_.missing;
  }
  else if (auto image = gfx::load_png(file)) {
    const double want =
        static_cast<double>(engine_size.w) / static_cast<double>(engine_size.h);
    const double have =
        static_cast<double>(image->width) / static_cast<double>(image->height);
    if (std::abs(have / want - 1.0) > aspect_tolerance) {
      Logger::warn("HD pack: {} is {}x{} but the original is {}x{} (aspect "
                   "ratio differs); ignoring it",
                   relative, image->width, image->height, engine_size.w / 2,
                   engine_size.h / 2);
      ++stats_.rejected;
    }
    else {
      result = std::make_shared<const gfx::HdImage>(
          gfx::resample(*image, target_w, target_h));
      ++stats_.loaded;
    }
  }
  else {
    ++stats_.rejected;
  }
  cache_.emplace(cache_key, result);
  return result;
}

} // namespace resources
