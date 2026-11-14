/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
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
namespace fx {

class MapStore {
public:
  static MapStore& instance();

  // An empty root falls back to <HD pack root>/_maps.
  void configure(std::filesystem::path root);
  [[nodiscard]] const std::filesystem::path& root();
  // True when there is a maps folder at all.
  [[nodiscard]] bool enabled();

  // The picture's maps, the picture's size, or null.
  [[nodiscard]] std::shared_ptr<const gfx::HdImage>
  emissive(const gfx::HdImage* picture);
  [[nodiscard]] std::shared_ptr<const gfx::HdImage>
  normal(const gfx::HdImage* picture);

  // Names a picture by its asset id. Pictures the HD pack loaded are found
  // by themselves (see sync()); others (tests, made-up pictures) can be
  // named here.
  void bind(const std::shared_ptr<const gfx::HdImage>& picture,
            std::string asset_id);
  // Learns the asset ids of the pictures the HD pack loaded since the last
  // call (cheap when nothing new was loaded). Call once per frame.
  void sync();
  // Forgets everything (tests; a new pack).
  void clear();

private:
  struct Named {
    std::weak_ptr<const gfx::HdImage> picture;
    std::string id;
  };
  struct Maps {
    bool tried_emissive = false;
    bool tried_normal = false;
    std::shared_ptr<const gfx::HdImage> emissive;
    std::shared_ptr<const gfx::HdImage> normal;
  };
  const Named* named(const gfx::HdImage* picture);
  std::shared_ptr<const gfx::HdImage>
  load(const std::string& id, const char* file, const gfx::HdImage& picture);

  std::filesystem::path configured_;
  std::filesystem::path root_;
  bool resolved_ = false;
  bool enabled_ = false;
  std::size_t synced_ = 0;
  std::unordered_map<const gfx::HdImage*, Named> names_;
  std::unordered_map<const gfx::HdImage*, Maps> maps_;
};

} // namespace fx
