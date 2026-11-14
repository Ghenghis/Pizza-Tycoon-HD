/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/MapStore.h"

#include <cmath>
#include <string_view>
#include <system_error>
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
  if (image->width != picture.width || image->height != picture.height) {
    return std::make_shared<const gfx::HdImage>(
        gfx::resample(*image, picture.width, picture.height));
  }
  return std::make_shared<const gfx::HdImage>(std::move(*image));
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
