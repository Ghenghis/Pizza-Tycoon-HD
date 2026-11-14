/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/restaurant/FurniturePlacement.h"

#include <cstddef>
#include <cstdlib>

#include "core/restaurant/Furniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"
#include "core/util/LegacyConstants.h"
#include "engine/Fatal.h"
#include "engine/FormatCompat.h"
#include "resources/Bob.h"
#include "resources/BobManager.h"

namespace core::restaurant {

FurnitureSizeFn furniture_size_fn(BobManager& bobs) {
  Bob& furniture = bobs.get("bob/moebel.bob");
  Bob& kitchenware = bobs.get("bob/kuebobs.bob");
  return [&furniture, &kitchenware](const PlacedFurniture& pf) -> Size {
    const bool kitchen =
        pf.furniture()->category() == FurnitureCategory::KITCHENWARE;
    const int state = pf.bob_state();
    const auto& pictures = pf.bobs();
    // As BobRenderer::frame_index (BobRenderer.cpp:21-30).
    if (state < 0 || static_cast<std::size_t>(state) >= pictures.size()) {
      fatal(compat::format("Invalid render state with {} bigger than {} for {}",
                           state, pictures.size(),
                           kitchen ? "bob/kuebobs.bob" : "bob/moebel.bob"));
    }
    const auto frame = static_cast<std::size_t>(pictures[state]);
    return kitchen ? kitchenware.size(frame) : furniture.size(frame);
  };
}

WorldPoint interior_origin() {
  return WorldPoint{LegacyConstants::RESTAURANT_TILE_WIDTH,
                    LegacyConstants::RESTAURANT_TILE_HEIGHT};
}

bool goes_in_kitchen(const PlacedFurniture& piece) {
  return piece.furniture()->category() == FurnitureCategory::KITCHENWARE;
}

WorldRect candidate_rect(WorldPoint p, const PlacedFurniture& piece,
                         const FurnitureSizeFn& size) {
  const auto s = size(piece);
  // Furniture top-left should be at the click point
  return WorldRect{p.x, p.y, s.w, s.h};
}

bool intersects_wall(const WorldRect& r, Size room_size) {
  // Walls are 1 tile thick on all sides
  constexpr int tile_w = LegacyConstants::RESTAURANT_TILE_WIDTH;
  constexpr int tile_h = LegacyConstants::RESTAURANT_TILE_HEIGHT;

  // Interior rectangle in world coordinates
  const WorldRect interior{
      tile_w,               // x: after left wall
      tile_h,               // y: after top wall
      room_size.w * tile_w, // width: interior tiles only
      room_size.h * tile_h  // height: interior tiles only
  };

  return !interior.contains(r);
}

bool intersects_any_blocking_furniture(
    const WorldRect& candidate, const PlacedFurniture& piece,
    const std::vector<std::unique_ptr<PlacedFurniture>>& placed,
    const FurnitureSizeFn& size, const PlacedFurniture*& nearest_table_out,
    WorldRect& nearest_table_rect_out) {
  nearest_table_out = nullptr;
  nearest_table_rect_out = {};

  const auto* cand_f = piece.furniture();
  const auto cand_cat = cand_f->category();
  const auto cand_layer = cand_f->placement_layer();

  const auto dist_sq = [](WorldPoint a, WorldPoint b) {
    const int dx = a.x - b.x;
    const int dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
  };

  for (const auto& pf : placed) {
    const WorldRect other{pf->origin(), size(*pf)};

    if (!candidate.intersects(other)) {
      continue;
    }

    const auto other_cat = pf->furniture()->category();

    // CHAIR on TABLE:
    if (cand_cat == FurnitureCategory::CHAIR &&
        other_cat == FurnitureCategory::TABLE) {
      // Disallow if chair center lies inside the table
      if (other.contains(candidate.center())) {
        return true;
      }
      // Track nearest table center
      if (!nearest_table_out) {
        nearest_table_out = pf.get();
        nearest_table_rect_out = other;
      }
      else {
        const WorldPoint best_c = nearest_table_rect_out.center();
        const WorldPoint this_c = other.center();
        const WorldPoint cc = candidate.center();
        if (dist_sq(this_c, cc) < dist_sq(best_c, cc)) {
          nearest_table_out = pf.get();
          nearest_table_rect_out = other;
        }
      }
      continue;
    }

    // TABLE on CHAIR
    if (cand_cat == FurnitureCategory::TABLE &&
        other_cat == FurnitureCategory::CHAIR) {
      // Disallow if table center lies inside the chair
      if (other.contains(candidate.center())) {
        return true;
      }
      continue;
    }

    // DECOR (Top layer) on TABLE:
    if (cand_cat == FurnitureCategory::DECOR &&
        cand_layer == PlacementLayer::Top &&
        other_cat == FurnitureCategory::TABLE) {
      continue;
    }

    // Default: any other overlap blocks placement
    return true;
  }
  return false;
}

bool decor_conflicts_on_table(
    const WorldRect& candidate, const WorldRect& table_rect,
    const std::vector<std::unique_ptr<PlacedFurniture>>& placed,
    const FurnitureSizeFn& size) {
  for (const auto& up : placed) {
    const auto* of = up->furniture();
    if (of->category() != FurnitureCategory::DECOR ||
        of->placement_layer() != PlacementLayer::Top) {
      continue;
    }
    const Size other_sz = size(*up);
    const WorldRect other{up->origin().x, up->origin().y, other_sz.w,
                          other_sz.h};
    if (!other.intersects(table_rect)) {
      continue; // decor not on this table
    }
    if (candidate.intersects(other)) {
      return true; // conflicts with decor already on this table
    }
  }
  return false;
}

Rotation rotation_towards(const WorldRect& from, const WorldRect& to) {
  const WorldPoint fc = from.center();
  const WorldPoint tc = to.center();
  const WorldDelta wd = tc - fc;

  if (std::abs(wd.dx) >= std::abs(wd.dy)) {
    return (wd.dx >= 0) ? Rotation::EAST : Rotation::WEST;
  }
  return (wd.dy >= 0) ? Rotation::SOUTH : Rotation::NORTH;
}

PlaceResult place_furniture(Restaurant& restaurant,
                            std::unique_ptr<PlacedFurniture>& piece,
                            WorldPoint p, const FurnitureSizeFn& size) {
  auto& layout = *restaurant.layout();
  const bool kitchen = goes_in_kitchen(*piece);
  const auto& placed =
      kitchen ? layout.placed_kitchen_furniture() : layout.placed_furniture();
  const Size room = kitchen ? restaurant.kitchen_size() : restaurant.size();

  const WorldRect cand = candidate_rect(p, *piece, size);
  if (intersects_wall(cand, room)) {
    return PlaceResult::Wall;
  }
  const PlacedFurniture* nearest_table = nullptr;
  WorldRect nearest_table_rect{};
  if (intersects_any_blocking_furniture(cand, *piece, placed, size,
                                        nearest_table, nearest_table_rect)) {
    return PlaceResult::Blocked;
  }

  const auto category = piece->furniture()->category();
  if (nearest_table && category == FurnitureCategory::CHAIR) {
    piece->rotate(rotation_towards(cand, nearest_table_rect));
    piece->linked_furniture_id(nearest_table->id());
  }
  const core::FurnitureId new_id = piece->id();
  piece->origin(cand.point());
  if (kitchen) {
    layout.place_kitchen_furniture(std::move(piece));
  }
  else {
    layout.place_furniture(std::move(piece));
  }

  // A new table takes the chairs that overlap it (center outside).
  if (category == FurnitureCategory::TABLE) {
    for (const auto& pf : layout.placed_furniture()) {
      if (pf->furniture()->category() != FurnitureCategory::CHAIR) {
        continue;
      }
      const WorldRect chair_rect{pf->origin(), size(*pf)};
      const bool overlaps =
          chair_rect.intersects(cand) && !cand.contains(chair_rect.center());
      if (overlaps) {
        pf->linked_furniture_id(new_id);
      }
      else if (pf->linked_furniture_id() == new_id) {
        pf->unlink_furniture();
      }
    }
  }
  return PlaceResult::Placed;
}

} // namespace core::restaurant
