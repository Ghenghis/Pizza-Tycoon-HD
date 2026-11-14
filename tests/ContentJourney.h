/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The scripted first week in Santa's Pizza Village, shared by the journey
// tests and the Expanded golden journey: the console commands of --exec runs
// and the core calls behind them, with Catch checks along the way.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "ContentTestGame.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/debug/command/CommandRegistry.h"
#include "core/debug/command/CommandResult.h"
#include "core/debug/commands/CityCommands.h"
#include "core/debug/commands/GameCommands.h"
#include "core/debug/commands/NavCommands.h"
#include "core/debug/commands/RestaurantCommands.h"
#include "core/debug/commands/StaffCommands.h"
#include "core/factories/FurnitureFactory.h"
#include "core/food/PlayerRecipes.h"
#include "core/restaurant/Furniture.h"
#include "core/restaurant/FurniturePlacement.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantLayout.h"
#include "engine/FormatCompat.h"

namespace content_journey {

inline constexpr std::uint32_t journey_seed = 0x5A7A;

inline debug::command::CommandRegistry& console() {
  static debug::command::CommandRegistry registry = [] {
    debug::command::CommandRegistry r;
    r.register_command(debug::commands::make_city_commands());
    r.register_command(debug::commands::make_restaurant_commands());
    r.register_command(debug::commands::make_staff_commands());
    r.register_command(debug::commands::make_game_commands());
    r.register_command(debug::commands::make_nav_commands());
    return r;
  }();
  return registry;
}

inline debug::command::CommandResult try_run(content_test::TestGame& game,
                                             const std::string& line) {
  return console().execute(line, game.state, game.context);
}

inline void run(content_test::TestGame& game, const std::string& line) {
  INFO("console: " << line);
  const auto result = try_run(game, line);
  INFO((result.error_message ? *result.error_message : std::string{}));
  REQUIRE(result.status == debug::command::CommandStatus::Success);
}

// Which restaurant the person trades in.
enum class Fit {
  // The quick start's own, furnished from its layout file.
  QuickStartLayout,
  // The rented second premise, furnished piece by piece with
  // `restaurant place`.
  PieceByPiece,
};

struct Journey {
  Restaurant* shop{nullptr};
  Restaurant* second{nullptr};
  Restaurant* trading{nullptr};
};

// Two tables with a chair on each side and an oven, placed one by one by
// the placement rules the restaurant view uses. The sizes are the drawn
// ones, so the positions follow the sprites. (The game has no counter piece:
// furniture.json has tables, chairs, decor, entertainment, games, slot
// machines and kitchenware.)
inline void furnish_piece_by_piece(content_test::TestGame& game,
                                   Restaurant& r) {
  namespace placement = core::restaurant;
  auto& state = game.state;
  REQUIRE(r.layout());
  CHECK(r.layout()->placed_furniture().empty());
  CHECK(r.layout()->placed_kitchen_furniture().empty());
  state.active_restaurant(r.id());

  const auto size = placement::furniture_size_fn(game.bobs);
  const auto* table =
      FurnitureFactory::create_furniture(FurnitureType::ROUND_GRILL);
  const auto* chair =
      FurnitureFactory::create_furniture(FurnitureType::TRADITIO_ROMANO);
  REQUIRE(table);
  REQUIRE(chair);
  const PlacedFurniture table_probe(table, core::FurnitureId{0},
                                    WorldPoint{0, 0},
                                    core::FurnitureId{UNLINKED_FURNITURE}, 0);
  const PlacedFurniture chair_probe(chair, core::FurnitureId{0},
                                    WorldPoint{0, 0},
                                    core::FurnitureId{UNLINKED_FURNITURE}, 0);
  const Size ts = size(table_probe);
  const Size cs = size(chair_probe);
  INFO("table " << ts.w << "x" << ts.h << ", chair " << cs.w << "x" << cs.h
                << ", room " << r.size().w << "x" << r.size().h << " tiles");
  REQUIRE(ts.w > 0);
  REQUIRE(cs.w > 0);

  const int overlap = std::max(1, cs.w / 4);
  const int tx = cs.w + 4;
  for (int k = 0; k < 2; ++k) {
    const int ty = 4 + (k * (std::max(ts.h, cs.h) + 12));
    run(game, compat::format("restaurant place ROUND_GRILL {} {}", tx, ty));
    // Chairs overlap the table edge with their centre outside it, so they
    // face and join that table.
    run(game, compat::format("restaurant place TRADITIO_ROMANO {} {}",
                             tx - cs.w + overlap, ty));
    run(game, compat::format("restaurant place TRADITIO_ROMANO {} {}",
                             tx + ts.w - overlap, ty));
    // The same table again on top of the first is refused.
    CHECK(try_run(game,
                  compat::format("restaurant place ROUND_GRILL {} {}", tx, ty))
              .status != debug::command::CommandStatus::Success);
  }
  // Outside the walls is refused too.
  CHECK(try_run(game, "restaurant place ROUND_GRILL 1999 1999").status !=
        debug::command::CommandStatus::Success);
  run(game, "restaurant place OVEN_SMALL 0 0");

  const auto& dining = r.layout()->placed_furniture();
  REQUIRE(dining.size() == 6);
  REQUIRE(r.layout()->placed_kitchen_furniture().size() == 1);
  for (const auto& pf : dining) {
    if (pf->furniture()->category() == FurnitureCategory::CHAIR) {
      CHECK(pf->linked_furniture_id().has_value());
    }
  }
}

// Start, rent, furnish, menu, stock, staff and open: everything before
// trading.
inline Journey set_up_shop(content_test::TestGame& game,
                           Fit fit = Fit::QuickStartLayout) {
  const auto scenario = content_test::load_cozy_village();
  const auto village = content_test::load_village();
  content_test::start_village_game(game, journey_seed, scenario, village);
  auto& state = game.state;
  content_test::close_dialogs(state);

  Player& me = *state.player(core::PlayerId{0});
  REQUIRE(me.is_human_controlled());
  City& city = *state.city(core::CityId{0});
  REQUIRE(city.name() == village->name);

  Journey j;
  // The quick start rented a premise and furnished it from a layout file.
  auto mine = state.restaurants_for(me.id());
  REQUIRE(mine.size() == 1);
  j.shop = mine[0];
  CHECK(&j.shop->property().city() == &city);
  CHECK(j.shop->property().owner_or_tenant() == me.id());
  REQUIRE(j.shop->layout());
  CHECK_FALSE(j.shop->layout()->placed_furniture().empty());
  CHECK_FALSE(j.shop->layout()->placed_kitchen_furniture().empty());

  // Rent the cheapest free premise from its realtor, as the console's
  // `city rent` does in scripted runs. The person starts with the original
  // character's $8,000.
  Property* to_rent = nullptr;
  for (Property* p : city.business_premises()) {
    if (p->is_for_rent() && !p->has_restaurant() &&
        (!to_rent || p->get_monthly_rent() < to_rent->get_monthly_rent())) {
      to_rent = p;
    }
  }
  REQUIRE(to_rent != nullptr);
  const Money rent = to_rent->get_monthly_rent();
  REQUIRE(rent <= me.cash_balance());
  const Money cash_before_rent = me.cash_balance();
  run(game, "city rent " + std::to_string(to_rent->index()));
  CHECK(me.cash_balance() == cash_before_rent - rent);
  CHECK(to_rent->owner_or_tenant() == me.id());
  REQUIRE(to_rent->restaurant_id().has_value());
  j.second = state.restaurant(*to_rent->restaurant_id());
  REQUIRE(j.second != nullptr);
  CHECK(state.restaurants_for(me.id()).size() == 2);

  j.trading = j.shop;
  if (fit == Fit::PieceByPiece) {
    furnish_piece_by_piece(game, *j.second);
    j.trading = j.second;
  }

  // The quick start's standard pizzas go on the menu.
  int on_menu = 0;
  for (const auto& slot : me.recipes().standard) {
    if (slot.created()) {
      j.trading->menu().add_or_set(slot.recipe->id(), Money{800});
      ++on_menu;
    }
  }
  REQUIRE(on_menu > 0);
  CHECK(j.trading->menu().count_total() == static_cast<std::size_t>(on_menu));

  // Stock the kitchen and hire a cook and a waiter.
  const std::string rid = std::to_string(j.trading->id().value);
  const Money cash_before_stock = me.cash_balance();
  run(game, "restaurant stock fill 10 @ " + rid);
  CHECK(me.cash_balance() < cash_before_stock);
  run(game, "staff hire 0 @ " + rid + " 20");
  run(game, "staff hire 1 @ " + rid + " 21");

  if (!j.trading->wants_open()) {
    j.trading->open_for_business(state);
  }
  CHECK(j.trading->wants_open());
  return j;
}

} // namespace content_journey
