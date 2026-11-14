/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/restaurant/PlacedFurniture.h"
#include "engine/Size.h"
#include "engine/geom/Spaces.h"

#include "resources/BobManager.h"
class Restaurant;

// Where furniture may stand in a restaurant: the rules of the furniture
// placement mode (views/screens/restaurant/FurniturePlacementController),
// without a view, so that scripted runs and tests can furnish a room piece by
// piece. The controller calls these same functions.
namespace core::restaurant {

// The size of a piece as it is drawn, in world pixels.
using FurnitureSizeFn = std::function<Size(const PlacedFurniture&)>;

// Sizes from the sprite sheets the restaurant view draws with
// (RestaurantView.cpp:68-69, 1267-1271): kitchenware from bob/kuebobs.bob,
// everything else from bob/moebel.bob, the frame of the piece's current
// picture. The manager must outlive the function.
[[nodiscard]] FurnitureSizeFn furniture_size_fn(BobManager& bobs);

// The world point of the first interior pixel: walls are one tile thick.
[[nodiscard]] WorldPoint interior_origin();

// The room a piece goes in: the kitchen for kitchenware, else the dining room.
[[nodiscard]] bool goes_in_kitchen(const PlacedFurniture& piece);

// The rectangle the piece covers with its top-left at p.
[[nodiscard]] WorldRect candidate_rect(WorldPoint p,
                                       const PlacedFurniture& piece,
                                       const FurnitureSizeFn& size);

// Whether r leaves the interior of a room of room_size tiles.
[[nodiscard]] bool intersects_wall(const WorldRect& r, Size room_size);

// Whether the candidate overlaps a piece it may not overlap. Chairs may
// overlap tables (and tables chairs) as long as the centre stays outside;
// top-layer decor may stand on tables. For a chair, the nearest overlapped
// table is reported.
[[nodiscard]] bool intersects_any_blocking_furniture(
    const WorldRect& candidate, const PlacedFurniture& piece,
    const std::vector<std::unique_ptr<PlacedFurniture>>& placed,
    const FurnitureSizeFn& size, const PlacedFurniture*& nearest_table_out,
    WorldRect& nearest_table_rect_out);

// Whether decor at candidate would overlap other top-layer decor already on
// the table at table_rect.
[[nodiscard]] bool decor_conflicts_on_table(
    const WorldRect& candidate, const WorldRect& table_rect,
    const std::vector<std::unique_ptr<PlacedFurniture>>& placed,
    const FurnitureSizeFn& size);

// The way a chair at from faces a table at to.
[[nodiscard]] Rotation rotation_towards(const WorldRect& from,
                                        const WorldRect& to);

enum class PlaceResult {
  Placed,
  // Outside the room's interior.
  Wall,
  // Overlaps a piece it may not overlap.
  Blocked,
};

// Places piece with its top-left at p in restaurant's dining room or kitchen,
// as a click in the placement mode does: a chair faces and links to the
// nearest table it overlaps, a table links the chairs that overlap it. On
// success piece is moved into the layout; otherwise it stays with the caller.
PlaceResult place_furniture(Restaurant& restaurant,
                            std::unique_ptr<PlacedFurniture>& piece,
                            WorldPoint p, const FurnitureSizeFn& size);

} // namespace core::restaurant
