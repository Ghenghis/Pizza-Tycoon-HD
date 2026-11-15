/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/MapStore.h"

#include <algorithm>
#include <array>
#include <cmath>
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

void MapStore::configure(std::filesystem::path root) {
  clear();
  configured_ = std::move(root);
}

void MapStore::clear() {
  names_.clear();
  maps_.clear();
  synced_ = 0;
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

void MapStore::bind(const std::shared_ptr<const gfx::HdImage>& picture,
                    std::string asset_id) {
  if (!picture) {
    return;
  }
  names_[picture.get()] = Named{picture, std::move(asset_id)};
  maps_.erase(picture.get());
}

void MapStore::sync() {
  if (!enabled()) {
    return;
  }
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

std::shared_ptr<const gfx::HdImage>
MapStore::load(const std::string& id, const char* file,
               const gfx::HdImage& picture) {
  const std::filesystem::path path = root() / id / file;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return nullptr;
  }
  std::string error;
  auto image = gfx::load_png(path, {}, &error);
  if (!image) {
    Logger::warn("HD maps: {}; ignoring it", error);
    return nullptr;
  }
  const double want = static_cast<double>(picture.width) / picture.height;
  const double have = static_cast<double>(image->width) / image->height;
  if (std::abs(have / want - 1.0) > resources::HdPack::aspect_tolerance) {
    Logger::warn("HD maps: {}/{} has another shape than its picture; "
                 "ignoring it",
                 id, file);
    return nullptr;
  }
  gfx::HdImage map =
      image->width != picture.width || image->height != picture.height
          ? gfx::resample(*image, picture.width, picture.height)
          : std::move(*image);
  if (std::string_view(file) == "emissive.png") {
    feather(map, emissive_feather_radius(gfx::hd_scale()));
  }
  return std::make_shared<const gfx::HdImage>(std::move(map));
}

std::shared_ptr<const gfx::HdImage>
MapStore::emissive(const gfx::HdImage* picture) {
  if (!picture || names_.empty()) {
    return nullptr;
  }
  const Named* n = named(picture);
  if (!n) {
    return nullptr;
  }
  Maps& m = maps_[picture];
  if (!m.tried_emissive) {
    m.tried_emissive = true;
    if (enabled()) {
      m.emissive = load(n->id, "emissive.png", *picture);
    }
  }
  return m.emissive;
}

std::shared_ptr<const gfx::HdImage>
MapStore::normal(const gfx::HdImage* picture) {
  if (!picture || names_.empty()) {
    return nullptr;
  }
  const Named* n = named(picture);
  if (!n) {
    return nullptr;
  }
  Maps& m = maps_[picture];
  if (!m.tried_normal) {
    m.tried_normal = true;
    if (enabled()) {
      m.normal = load(n->id, "normal.png", *picture);
    }
  }
  return m.normal;
}

} // namespace fx
