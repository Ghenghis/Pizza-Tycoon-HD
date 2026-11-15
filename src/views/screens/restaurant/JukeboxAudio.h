/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/restaurant/Furniture.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"

namespace views::screens::restaurant {

// The original song (1 to 6, 0 for none) the restaurant's jukebox plays:
// its jukebox_music while a jukebox stands in the dining room, not burnt to
// ashes. Sound only: jukebox_music itself, which the customers react to and
// the save keeps, is gameplay and is never changed here.
// Whether a jukebox stands in the dining room, not burnt to ashes.
inline bool has_jukebox(const Restaurant& restaurant) {
  if (!restaurant.layout()) {
    return false;
  }
  for (const auto& pf : restaurant.layout()->placed_furniture()) {
    if (pf && pf->furniture() && !pf->is_ashes() &&
        pf->furniture()->role() == FurnitureRole::Jukebox) {
      return true;
    }
  }
  return false;
}

inline int audible_jukebox_choice(const Restaurant& restaurant) {
  return has_jukebox(restaurant) ? restaurant.jukebox_music() : 0;
}

} // namespace views::screens::restaurant
