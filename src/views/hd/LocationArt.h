/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/common/Ids.h"
#include "engine/gfx/Surface.h"
#include "resources/VgaManager.h"

namespace core::content {
class Roster;
struct LocationDefinition;
} // namespace core::content

// Location-aware art (north-pole-art-plan.md section 1, route 2): the
// pictures a city's screens draw with, chosen by the city's location.
//
// A Classic city (one of the ten originals) draws with the 1994 keys and
// pictures exactly as before: the resolver hands back VgaManager's own
// picture, so the pixels, the HD art and the game are untouched.
//
// A location city keeps the 1994 indexed pixels (the tile ids, the 1994 look
// and the game never change) and takes its HD art from the HD pack's
// location folder when there is one:
//
//   <pack>/locations/<namespace>/gfx/landsym.png        the city map tiles
//   <pack>/locations/<namespace>/gfx/med1.png           the medium map
//   <pack>/locations/<namespace>/gfx/k3.png             a restaurant floor
//   <pack>/locations/<namespace>/districts/<district id>/gfx/k3.png
//                                                        one district's floor
//
// The namespace is the location's bonus route (north-pole) or its world
// region. A replacement follows the pack's rules for the 1994 picture it
// stands in for (its aspect ratio within HdPack::aspect_tolerance, resized
// to the engine size times hd_scale()); an atlas (landsym, med1) must also
// keep the 16 x 16 grid of its 256 tile ids: both sides a multiple of 16 and
// at least the original's. Anything missing or wrong falls back to the
// classic picture, with one log line per picture and scope.
namespace views::hd {

// Where a city's art comes from.
struct ArtScope {
  std::string ns;       // "" for Classic; else the location's namespace
  std::string district; // a district id of the location, or ""
  [[nodiscard]] bool classic() const { return ns.empty(); }
  bool operator==(const ArtScope&) const = default;
};

// The namespace of a location: its bonus route, else its region; "" when it
// has neither (it then draws as Classic).
[[nodiscard]] std::string
location_namespace(const core::content::LocationDefinition& definition);

// The scope of roster position `city`: Classic for an original city or an
// unknown position; a location's namespace, plus the id of district
// `district` (its index in the location) when given.
[[nodiscard]] ArtScope art_scope(const core::content::Roster& roster,
                                 core::CityId city,
                                 std::optional<int> district = std::nullopt);

enum class CityAtlas : uint8_t {
  Tiles,  // gfx/landsym: the close zoom, the water frames, the HD medium map
  Medium, // gfx/med1: the medium zoom
};

// The atlas key the city map asks for, "<namespace>:gfx/<name>":
// "classic:gfx/landsym" for a Classic city; for a location, the definition's
// binding (assets.tile_atlas or medium_atlas) with "classic:" read as the
// location's own namespace (the classic layout, the location's art). A
// bundle: key or an empty one resolves to the classic key (logged once).
[[nodiscard]] std::string atlas_key(const core::content::Roster& roster,
                                    core::CityId city, CityAtlas atlas);

// "<namespace>:gfx/<name>" split into its scope and the 1994 picture key
// ("gfx/<name>"); "classic" is the Classic scope. Nothing for a key of
// another form.
struct SplitKey {
  ArtScope scope;
  std::string picture;
};
[[nodiscard]] std::optional<SplitKey> split_atlas_key(std::string_view key);

// The picture to draw `picture` (a 1994 key, "gfx/landsym") with in
// `scope`, given that 1994 picture. Classic scope: `classic` itself. A
// location scope: a copy of `classic` with the location's HD art attached,
// or `classic` when there is none or it breaks the rules. Kept for the
// session (until clear_location_pictures()).
[[nodiscard]] const gfx::Surface& location_picture(const gfx::Surface& classic,
                                                   std::string_view picture,
                                                   const ArtScope& scope);

// The city map atlas for roster position `city`.
[[nodiscard]] const gfx::Surface&
city_atlas(VgaManager& vga, const core::content::Roster& roster,
           core::CityId city, CityAtlas atlas);

// A restaurant interior picture (floors gfx/k*, walls and panels gfx/back*)
// for a restaurant in district `district` of roster position `city`: the
// district's own, else the location's, else the classic one. Any other key
// is the classic picture.
[[nodiscard]] const gfx::Surface&
interior_picture(VgaManager& vga, const core::content::Roster& roster,
                 core::CityId city, std::optional<int> district,
                 std::string_view picture);

// Forgets the pictures made so far (a new HD pack; tests).
void clear_location_pictures();

} // namespace views::hd
