/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/hd/LocationArt.h"

#include <cmath>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "core/content/LocationDefinition.h"
#include "core/content/Roster.h"
#include "engine/Logger.h"
#include "engine/gfx/Png.h"
#include "resources/HdPack.h"

namespace views::hd {

namespace {

constexpr std::string_view classic_ns = "classic";

bool is_atlas(std::string_view picture) {
  return picture == "gfx/landsym" || picture == "gfx/med1";
}

bool is_interior(std::string_view picture) {
  return picture.starts_with("gfx/k") || picture.starts_with("gfx/back");
}

// A namespace or district id safe as one folder name.
bool folder_name(std::string_view s) {
  if (s.empty() || s.size() > 64) {
    return false;
  }
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

struct Made {
  std::optional<gfx::Surface> surface; // null: the classic picture
};

struct Store {
  std::unordered_map<std::string, Made> made;
  std::unordered_set<std::string> logged;
};

Store& store() {
  static Store s;
  return s;
}

void log_once(const std::string& what, const std::string& why) {
  if (store().logged.insert(what).second) {
    Logger::info("Location art {}: {}; using the classic picture", what, why);
  }
}

// Whether the pack file can stand in for `classic` (engine size `engine`):
// the empty string when it can, else why not.
std::string check_layout(const gfx::PngInfo& info, std::string_view picture,
                         Size engine) {
  const double want =
      static_cast<double>(engine.w) / static_cast<double>(engine.h);
  const double have =
      static_cast<double>(info.width) / static_cast<double>(info.height);
  if (std::abs(have / want - 1.0) > resources::HdPack::aspect_tolerance) {
    return "its aspect ratio is not the original's";
  }
  if (is_atlas(picture)) {
    // The 1994 picture is 16 x 16 tiles: every tile id must keep its place.
    const int ow = engine.w / gfx::asset_pixel_scale;
    const int oh = engine.h / gfx::asset_pixel_scale;
    if (info.width % 16 != 0 || info.height % 16 != 0 || info.width < ow ||
        info.height < oh) {
      return "it does not keep the 16 x 16 tile grid at the original size or "
             "more";
    }
  }
  return {};
}

} // namespace

std::string
location_namespace(const core::content::LocationDefinition& definition) {
  std::string ns =
      definition.bonus ? definition.bonus_route : definition.region;
  return folder_name(ns) ? ns : std::string{};
}

ArtScope art_scope(const core::content::Roster& roster, core::CityId city,
                   std::optional<int> district) {
  ArtScope scope;
  if (!roster.contains(city)) {
    return scope;
  }
  const auto& entry = roster.entry(city);
  if (entry.source != core::content::RosterSource::Definition ||
      !entry.definition) {
    return scope;
  }
  scope.ns = location_namespace(*entry.definition);
  if (!scope.ns.empty() && district && *district >= 0 &&
      static_cast<std::size_t>(*district) <
          entry.definition->districts.size()) {
    const std::string& id =
        entry.definition->districts[static_cast<std::size_t>(*district)].id;
    if (folder_name(id)) {
      scope.district = id;
    }
  }
  return scope;
}

std::optional<SplitKey> split_atlas_key(std::string_view key) {
  const auto colon = key.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return std::nullopt;
  }
  const std::string_view ns = key.substr(0, colon);
  const std::string_view picture = key.substr(colon + 1);
  if (!picture.starts_with("gfx/") || picture.size() <= 4 ||
      picture.size() > 20) {
    return std::nullopt;
  }
  for (const char c : picture.substr(4)) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return std::nullopt;
    }
  }
  SplitKey out;
  out.picture = std::string(picture);
  if (ns != classic_ns) {
    if (!folder_name(ns)) {
      return std::nullopt;
    }
    out.scope.ns = std::string(ns);
  }
  return out;
}

std::string atlas_key(const core::content::Roster& roster, core::CityId city,
                      CityAtlas atlas) {
  const std::string classic =
      atlas == CityAtlas::Tiles ? "classic:gfx/landsym" : "classic:gfx/med1";
  const ArtScope scope = art_scope(roster, city);
  if (scope.classic()) {
    return classic;
  }
  const auto& def = *roster.entry(city).definition;
  const std::string& bound =
      atlas == CityAtlas::Tiles ? def.tile_atlas : def.medium_atlas;
  const auto split = split_atlas_key(bound);
  if (!split) {
    log_once(std::string(def.key.str()) + " " + bound,
             bound.empty() ? "the location binds no atlas"
                           : "this build cannot load that kind of key");
    return classic;
  }
  // classic:gfx/landsym in a location: the classic layout, the location's
  // own art.
  const std::string ns = split->scope.classic() ? scope.ns : split->scope.ns;
  return ns + ":" + split->picture;
}

const gfx::Surface& location_picture(const gfx::Surface& classic,
                                     std::string_view picture,
                                     const ArtScope& scope) {
  if (scope.classic() || !folder_name(scope.ns)) {
    return classic;
  }
  auto& pack = resources::HdPack::instance();
  if (!pack.enabled()) {
    return classic; // no HD at all: nothing to choose
  }
  const std::string id = scope.ns + ":" + std::string(picture) +
                         (scope.district.empty() ? "" : "@" + scope.district);
  Store& s = store();
  if (const auto it = s.made.find(id); it != s.made.end()) {
    return it->second.surface ? *it->second.surface : classic;
  }
  Made made;
  std::string why = "the pack has no picture for it";
  std::vector<std::string> candidates;
  if (!scope.district.empty()) {
    candidates.push_back("locations/" + scope.ns + "/districts/" +
                         scope.district + "/" + std::string(picture));
  }
  candidates.push_back("locations/" + scope.ns + "/" + std::string(picture));
  for (const std::string& rel : candidates) {
    const std::filesystem::path file = pack.root() / (rel + ".png");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      continue;
    }
    std::string error;
    const auto info = gfx::read_png_info(file, {}, &error);
    if (!info) {
      why = rel + ".png: " + error;
      continue;
    }
    if (std::string bad = check_layout(*info, picture, classic.size());
        !bad.empty()) {
      why = rel + ".png: " + bad;
      continue;
    }
    auto hd = pack.vga(rel, classic.size());
    if (!hd) {
      why = rel + ".png did not load";
      continue;
    }
    made.surface = classic.clone();
    made.surface->attach_hd(std::move(hd));
    break;
  }
  if (!made.surface) {
    log_once(id, why);
  }
  const auto [it, inserted] = s.made.emplace(id, std::move(made));
  return it->second.surface ? *it->second.surface : classic;
}

const gfx::Surface& city_atlas(VgaManager& vga,
                               const core::content::Roster& roster,
                               core::CityId city, CityAtlas atlas) {
  const std::string key = atlas_key(roster, city, atlas);
  const auto split = split_atlas_key(key);
  // atlas_key() always gives a key of this form.
  const std::string picture =
      split ? split->picture
            : (atlas == CityAtlas::Tiles ? "gfx/landsym" : "gfx/med1");
  const gfx::Surface& classic = vga.get(picture);
  return split ? location_picture(classic, picture, split->scope) : classic;
}

const gfx::Surface& interior_picture(VgaManager& vga,
                                     const core::content::Roster& roster,
                                     core::CityId city,
                                     std::optional<int> district,
                                     std::string_view picture) {
  const gfx::Surface& classic = vga.get(picture);
  if (!is_interior(picture)) {
    return classic;
  }
  return location_picture(classic, picture, art_scope(roster, city, district));
}

void clear_location_pictures() {
  store().made.clear();
  store().logged.clear();
}

} // namespace views::hd
