/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/debug/commands/RestaurantCommands.h"

#include <algorithm>
#include <magic_enum/magic_enum.hpp>
#include <set>
#include <string>
#include <vector>

#include "core/character/Player.h"
#include "core/character/Staff.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/debug/command/CommonArgs.h"
#include "core/factories/CharacterFactory.h"
#include "core/factories/FurnitureFactory.h"
#include "core/factories/IngredientFactory.h"
#include "core/factories/PizzaFactory.h"
#include "core/factories/RestaurantLayoutFactory.h"
#include "core/finance/Ledger.h"
#include "core/food/IngredientCategory.h"
#include "core/food/IngredientCategoryType.h"
#include "core/food/PlayerRecipes.h"
#include "core/food/ToppingSize.h"
#include "core/restaurant/Furniture.h"
#include "core/restaurant/FurniturePlacement.h"
#include "core/restaurant/PlacedFurniture.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/RestaurantEconomy.h"
#include "core/restaurant/RestaurantLayout.h"
#include "core/state/GameState.h"
#include "core/state/RealEstate.h"
#include "core/supply/ActiveContract.h"
#include "core/supply/FoodSupplier.h"
#include "core/supply/IngredientBatch.h"
#include "core/supply/StockOrder.h"
#include "core/supply/SupplierContract.h"
#include "core/supply/SupplierRelationship.h"
#include "core/util/GameContext.h"

using namespace debug::command;

namespace debug::commands {

// The supplier sets the price and quality, and the player pays. Empty when
// the ingredient is not sold or the player can't afford it.
static std::optional<Money> buy_ingredient(GameState& state, Player& player,
                                           Restaurant& restaurant,
                                           const FoodSupplier& supplier,
                                           core::food::IngredientType type,
                                           int kg) {
  const auto& city = restaurant.property().city();
  auto price_per_kg = supplier.calculate_price(
      type, city.ingredient_base_price(type),
      FoodSupplier::price_multiplier(state.difficulty_level()));
  if (!price_per_kg) {
    return std::nullopt;
  }

  Money cost{price_per_kg->cents * kg};
  if (player.cash_balance() < cost) {
    return std::nullopt;
  }
  (void)player.spend_cash(state.time().current_date(), MonthlyCategory::Stock,
                          cost, restaurant.id());
  restaurant.inventory().add_ingredient(type, kg * 1000, supplier.quality(),
                                        *price_per_kg);
  return cost;
}

static CommandDef make_restaurant_info() {
  return CommandDef{
      .name = "info",
      .description = "Show restaurant information",
      .help_text = R"(Usage: restaurant info [player_id] [restaurant_id]

Displays detailed information about a restaurant including layout,
furniture count, and statistics.

Examples:
  restaurant info              # Active player's active restaurant
  restaurant info @            # Active player's active restaurant (explicit)
  restaurant info @ 0          # Active player's restaurant 0
  restaurant info 2 3          # Player 2's restaurant 3
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        // Check ownership via property
        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        std::vector<std::string> lines;
        lines.push_back(
            compat::format("Restaurant ID: {}", restaurant_id.value));
        lines.push_back(compat::format("Owner: Player {}", player_id.value));
        lines.push_back(compat::format("Name: {}", restaurant->name()));
        lines.push_back(compat::format(
            "Inspection in: {} day(s){}",
            restaurant->inspection_days_remaining(),
            restaurant->first_inspection_pending() ? ", first one" : ""));

        auto& layout = restaurant->layout();
        int furniture_count =
            static_cast<int>(layout->placed_furniture().size() +
                             layout->placed_kitchen_furniture().size());
        lines.push_back(compat::format("Furniture count: {}", furniture_count));
        lines.push_back(compat::format("Furniture value: ${:n}",
                                       layout->furniture_value()));
        lines.push_back(compat::format(
            "Ever opened: {}", restaurant->ever_opened() ? "yes" : "no"));
        lines.push_back(compat::format(
            "Popularity: {}", restaurant->economy().total_popularity()));
        const auto p = restaurant->popularity();
        lines.push_back(compat::format(
            "  waiting time {}, pizza prices {}, recipes on menu {}, "
            "advertising {}",
            p.waiting_time, p.pizza_prices, p.recipes_on_menu, p.advertising));
        lines.push_back(compat::format(
            "  star rating {}, furnishing {}, demand fulfillment {}, "
            "unexpected events {}",
            p.star_rating, p.furnishing, p.demand_fulfillment,
            p.unexpected_events));

        return CommandResult::success(lines);
      },
  };
}

static CommandDef make_restaurant_clear() {
  return CommandDef{
      .name = "clear",
      .description = "Remove all furniture from restaurant",
      .help_text = R"(Usage: restaurant clear [player_id] [restaurant_id]

Removes all furniture from the restaurant layout.

Examples:
  restaurant clear             # Clear active restaurant
  restaurant clear @ 0         # Clear restaurant 0 of active player
  restaurant clear 2 3         # Clear player 2's restaurant 3
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }
        restaurant->layout()->clear();

        // For now, clearing is not implemented as there's no clear() method
        // User can recreate the restaurant layout manually
        return CommandResult::success({"Restaurant furniture removed"});
      },
  };
}

static CommandDef make_restaurant_shutdown() {
  return CommandDef{
      .name = "shutdown",
      .description = "Shut a restaurant down and give up its premise",
      .help_text = R"(Usage: restaurant shutdown [player_id] [restaurant_id]

Sells the premise, or ends the lease if it is rented, which takes the
restaurant with it. The owner is paid nothing for a premise they own.
To stop trading without losing the premise, use 'restaurant close'.

Examples:
  restaurant shutdown          # Shut down the active restaurant
  restaurant shutdown 3 1      # Shut down player 3's restaurant 1
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto* player = ctx.state.player(player_id);
        if (!player) {
          return CommandResult::error(
              compat::format("Player {} does not exist", player_id.value));
        }

        auto& property = restaurant->property();
        auto owner = property.owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        const bool given_up =
            property.is_rented()
                ? ctx.state.real_estate().end_property_lease(property, *player)
                : ctx.state.real_estate().sell_property(property, *player,
                                                        Money{0});
        if (!given_up) {
          return CommandResult::error(compat::format(
              "Restaurant {} could not be shut down", restaurant_id.value));
        }

        return CommandResult::success().add_message(
            compat::format("Shut down restaurant {}", restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_open() {
  return CommandDef{
      .name = "open",
      .description = "Open a restaurant for business",
      .help_text = R"(Usage: restaurant open [player_id] [restaurant_id]

Opens the restaurant, so it serves customers within its business hours.
A restaurant an inspection has shut stays shut until its days run out.

Examples:
  restaurant open              # Open the active restaurant
  restaurant open 3 1          # Open player 3's restaurant 1
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        if (restaurant->is_force_closed()) {
          return CommandResult::error(compat::format(
              "Restaurant {} is force-closed", restaurant_id.value));
        }

        restaurant->open_for_business(ctx.state);
        return CommandResult::success().add_message(compat::format(
            "Restaurant {} is open for business", restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_close() {
  return CommandDef{
      .name = "close",
      .description = "Close a restaurant for business (see shutdown to get rid "
                     "of the restaurant)",
      .help_text = R"(Usage: restaurant close [player_id] [restaurant_id]

Closes the restaurant to customers and leaves everything else alone. To give
up the premise as well, use 'restaurant shutdown'.

Examples:
  restaurant close             # Close the active restaurant
  restaurant close 3 1         # Close player 3's restaurant 1
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        restaurant->close_for_business();
        return CommandResult::success().add_message(
            compat::format("Restaurant {} is closed", restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_ai_layout() {
  return CommandDef{
      .name = "ai-layout",
      .description = "Replace a restaurant's layout with a prebuilt AI one",
      .help_text =
          R"(Usage: restaurant ai-layout <index> [player_id] [restaurant_id]

Loads f/f<index>.pc over the restaurant's own layout, furniture and all, so a
prebuilt AI interior can be walked through. The room takes the layout's size,
so leave the restaurant view before running this and go back in afterwards.

Indices 1 to 18 are the ones an AI picks from; 19 to 24 exist as files but are
never used.

Examples:
  restaurant ai-layout 1        # Active restaurant gets the first AI layout
  restaurant ai-layout 19 @ 0   # Restaurant 0, one of the unreachable layouts
)",
      .args =
          {
              ArgSpec{
                  .name = "index",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "AI layout number",
                  .min_value = 1,
                  .max_value = RestaurantLayoutFactory::ai_layout_count,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const int index = static_cast<int>(ctx.int_arg(0));
        auto player_id = ctx.player_id(1);
        auto restaurant_id = ctx.restaurant_id(2);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        // More furniture than any layout file holds, so none of it is dropped.
        constexpr int every_item = 255;
        if (!restaurant->ai_layout(index, every_item)) {
          return CommandResult::error(
              compat::format("AI layout {} could not be loaded", index));
        }

        const auto size = restaurant->size();
        return CommandResult::success().add_message(
            compat::format("Restaurant {} now has AI layout {} ({}x{})",
                           restaurant_id.value, index, size.w, size.h));
      },
  };
}

static CommandDef make_restaurant_furniture_list() {
  return CommandDef{
      .name = "list",
      .description = "List all furniture in restaurant",
      .help_text =
          R"(Usage: restaurant furniture list [player_id] [restaurant_id]

Lists all furniture items placed in the restaurant.

Examples:
  restaurant furniture list        # Active restaurant
  restaurant furniture list @ 0    # Restaurant 0 of active player
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        auto& layout = restaurant->layout();
        int furniture_count =
            static_cast<int>(layout->placed_furniture().size() +
                             layout->placed_kitchen_furniture().size());
        std::vector<std::string> lines;
        lines.push_back(
            compat::format("Furniture in restaurant {}:", restaurant_id.value));
        lines.push_back(compat::format("Total items: {}", furniture_count));
        lines.push_back(
            compat::format("Total value: ${:n}", layout->furniture_value()));

        return CommandResult::success(lines);
      },
  };
}

static CommandDef make_restaurant_furniture() {
  return CommandDef{
      .name = "furniture",
      .description = "Manage restaurant furniture",
      .help_text = "Subcommands: list",
  }
      .add_subcommand(make_restaurant_furniture_list());
}

// Restocking as the order screen does: one order per category from the
// restaurant's usual supplier (its preferred one, or the first in the city),
// at that supplier's price and quality, through buy_for_restaurant (the
// payment, the stock, the supplier's stock and spend, the preferred
// supplier). Each ingredient gets kg, up to the restaurant's stock limit;
// ingredients the supplier does not sell are skipped. On success the money
// spent and the ingredients bought; otherwise why not.
struct Restock {
  Money spent{0};
  int ingredients = 0;
  std::string error;
};

static Restock restock_from_usual_suppliers(
    GameState& state, Player& player, Restaurant& restaurant,
    const std::set<core::food::IngredientType>& types, int kg) {
  Restock result;
  auto& city = restaurant.property().city();
  for (auto category : ALL_INGREDIENT_CATEGORIES) {
    const FoodSupplier* usual = restaurant.supplier_for(city, category);
    if (!usual) {
      continue;
    }
    FoodSupplier* supplier = nullptr;
    for (auto* s : city.food_suppliers(
             core::food::CategoryRegistry::supplier_type(category))) {
      if (s->id() == usual->id() && s->category() == category) {
        supplier = s;
        break;
      }
    }
    if (!supplier) {
      continue;
    }
    std::vector<IngredientBatch> order;
    for (auto type : core::food::CategoryRegistry::range(category)) {
      if (!types.contains(type) || !supplier->sells(type)) {
        continue;
      }
      const auto price = supplier->calculate_price(
          type, city.ingredient_base_price(type),
          FoodSupplier::price_multiplier(state.difficulty_level()));
      if (!price) {
        continue;
      }
      const auto* stock = restaurant.inventory().get_ingredient_stock(type);
      const int current_kg = stock ? stock->quantity / 1000 : 0;
      const int count =
          std::min(kg, std::max(0, max_ingredient_stock_kg - current_kg));
      if (count <= 0) {
        continue;
      }
      IngredientBatch batch;
      batch.ingredient =
          IngredientFactory::create_ingredient(static_cast<int>(type));
      batch.ingredient_type = type;
      batch.count = count;
      batch.price_per_unit = *price;
      batch.quality = supplier->quality();
      batch.current_stock = current_kg;
      order.push_back(batch);
    }
    if (order.empty()) {
      continue;
    }
    const Money cost = order_total(order);
    if (!buy_for_restaurant(state, player, city, restaurant, supplier, order)) {
      result.error = compat::format(
          "Player {} can't afford ${:n} from {} after {} ingredients (${:n} "
          "spent)",
          player.id().value, cost, supplier->name(), result.ingredients,
          result.spent);
      return result;
    }
    result.spent = result.spent + cost;
    result.ingredients += static_cast<int>(order.size());
  }
  return result;
}

static CommandDef make_restaurant_stock_buy() {
  return CommandDef{
      .name = "buy",
      .description = "Buy stock for one ingredient from a supplier",
      .help_text =
          R"(Usage: restaurant stock buy <supplier_id> <ingredient_id> <kg> [player_id] [restaurant_id]

Buys stock from a supplier in the restaurant's city. The supplier sets the
price and the quality, and the player pays, so the stock ends up costing what
stock bought in play costs. Fails if the player can't afford it.

Arguments:
  supplier_id   - Supplier to buy from; `city suppliers` lists them
  ingredient_id - Ingredient ID (0-21, 24-40, 48-60, 72-84)
  kg            - Kilograms to buy
  player_id     - Player ID (@ for active player)
  restaurant_id - Restaurant ID (@ for active restaurant)

Examples:
  restaurant stock buy 1 0 10     # 10kg of ingredient 0 from supplier 1
  restaurant stock buy 7 25 20    # 20kg of ingredient 25 from supplier 7
)",
      .args =
          {
              ArgSpec{
                  .name = "supplier_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Supplier id, from `city suppliers`",
                  .min_value = 0,
                  .max_value = 255,
              },
              ArgSpec{
                  .name = "ingredient_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Ingredient ID (0-89)",
                  .min_value = 0,
                  .max_value = 89,
              },
              ArgSpec{
                  .name = "kg",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Kilograms to buy",
                  .min_value = 1,
                  .max_value = 650,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto wanted_supplier = static_cast<uint8_t>(ctx.int_arg(0));
        auto ingredient_id = ctx.int_arg(1);
        auto kg = static_cast<int>(ctx.int_arg(2));
        auto player_id = ctx.player_id(3);
        auto restaurant_id = ctx.restaurant_id(4);

        auto* player = ctx.state.player(player_id);
        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!player || !restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        auto category = core::food::CategoryRegistry::from_ingredient_id(
            static_cast<int>(ingredient_id));
        if (!category) {
          return CommandResult::error(
              compat::format("{} is not an ingredient id", ingredient_id));
        }
        auto ingredient_type =
            static_cast<core::food::IngredientType>(ingredient_id);

        const FoodSupplier* supplier = nullptr;
        for (const auto& s : restaurant->property().city().suppliers()) {
          if (s->category() == *category && s->id() == wanted_supplier) {
            supplier = s.get();
            break;
          }
        }
        if (!supplier) {
          return CommandResult::error(compat::format(
              "No supplier {} for that category in {}", wanted_supplier,
              restaurant->property().city().name()));
        }
        if (!supplier->sells(ingredient_type)) {
          return CommandResult::error(
              compat::format("{} does not sell ingredient {}", supplier->name(),
                             ingredient_id));
        }

        auto spent = buy_ingredient(ctx.state, *player, *restaurant, *supplier,
                                    ingredient_type, kg);
        if (!spent) {
          return CommandResult::error(
              compat::format("Player {} can't afford {}kg of ingredient {}",
                             player_id.value, kg, ingredient_id));
        }

        return CommandResult::success().add_message(compat::format(
            "Bought {}kg of ingredient {} from {} for ${:n} at quality {}", kg,
            ingredient_id, supplier->name(), *spent, supplier->quality()));
      },
  };
}

static CommandDef make_restaurant_stock_fill() {
  return CommandDef{
      .name = "fill",
      .description = "Buy the same weight of every ingredient sold",
      .help_text =
          R"(Usage: restaurant stock fill <kg> [player_id] [restaurant_id]

Buys the given weight of every ingredient from the restaurant's usual
supplier in each category. The supplier sets the price and the quality, and
the player pays. Stops with an error if the player runs out of money
partway. Ingredients that supplier does not sell are skipped.

Arguments:
  kg            - Kilograms to buy of each ingredient
  player_id     - Player ID (@ for active player)
  restaurant_id - Restaurant ID (@ for active restaurant)

Examples:
  restaurant stock fill 10        # 10kg of everything
  restaurant stock fill 20        # 20kg of everything
)",
      .args =
          {
              ArgSpec{
                  .name = "kg",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Kilograms of each ingredient",
                  .min_value = 1,
                  .max_value = 650,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto kg = static_cast<int>(ctx.int_arg(0));
        auto player_id = ctx.player_id(1);
        auto restaurant_id = ctx.restaurant_id(2);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        auto* player = ctx.state.player(player_id);
        if (!player) {
          return CommandResult::error(
              compat::format("Player {} not found", player_id.value));
        }

        const auto& city = restaurant->property().city();
        Money spent{0};
        int bought = 0;
        for (auto category : ALL_INGREDIENT_CATEGORIES) {
          const auto* supplier = restaurant->supplier_for(city, category);
          if (!supplier) {
            continue;
          }
          for (auto type : core::food::CategoryRegistry::range(category)) {
            if (!supplier->sells(type)) {
              continue;
            }
            auto cost = buy_ingredient(ctx.state, *player, *restaurant,
                                       *supplier, type, kg);
            if (!cost) {
              return CommandResult::error(
                  compat::format("Player {} ran out of money after {} "
                                 "ingredients (${:n} spent)",
                                 player_id.value, bought, spent));
            }
            spent = spent + *cost;
            ++bought;
          }
        }

        return CommandResult::success().add_message(compat::format(
            "Bought {}kg of each of {} ingredients for ${:n} for restaurant {}",
            kg, bought, spent, restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_stock_ingredient() {
  return CommandDef{
      .name = "ingredient",
      .description = "Buy one ingredient from the usual supplier",
      .help_text =
          R"(Usage: restaurant stock ingredient <ingredient_id> <kg> [player_id] [restaurant_id]

Orders one ingredient from the restaurant's usual supplier for its category
(the preferred one, or the first in the city), as the order screen does: the
supplier sets the price and the quality, and the player pays. Never more
than the restaurant's stock limit. Fails if the player can't afford it or
the supplier does not sell it.

Examples:
  restaurant stock ingredient 72 5    # 5kg of ingredient 72 (a cheese)
)",
      .args =
          {
              ArgSpec{
                  .name = "ingredient_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .default_provider = DefaultProvider::None,
                  .description = "Ingredient ID (0-89)",
                  .min_value = 0,
                  .max_value = 89,
              },
              ArgSpec{
                  .name = "kg",
                  .type = ArgType::Int,
                  .optional = false,
                  .default_provider = DefaultProvider::None,
                  .description = "Kilograms to buy",
                  .min_value = 1,
                  .max_value = max_ingredient_stock_kg,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const auto ingredient_id = static_cast<int>(ctx.int_arg(0));
        const auto kg = static_cast<int>(ctx.int_arg(1));
        auto player_id = ctx.player_id(2);
        auto restaurant_id = ctx.restaurant_id(3);
        auto* player = ctx.state.player(player_id);
        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!player || !restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }
        const auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }
        if (!core::food::CategoryRegistry::from_ingredient_id(ingredient_id)) {
          return CommandResult::error(
              compat::format("{} is not an ingredient id", ingredient_id));
        }
        const auto r = restock_from_usual_suppliers(
            ctx.state, *player, *restaurant,
            {static_cast<core::food::IngredientType>(ingredient_id)}, kg);
        if (!r.error.empty()) {
          return CommandResult::error(r.error);
        }
        if (r.ingredients == 0) {
          return CommandResult::error(compat::format(
              "Nothing bought: the usual supplier does not sell ingredient {}, "
              "or the stock is full",
              ingredient_id));
        }
        return CommandResult::success().add_message(
            compat::format("Bought up to {}kg of ingredient {} for ${:n}", kg,
                           ingredient_id, r.spent));
      },
  };
}

static CommandDef make_restaurant_stock_menu() {
  return CommandDef{
      .name = "menu",
      .description = "Buy what the menu's pizzas use, from the usual suppliers",
      .help_text =
          R"(Usage: restaurant stock menu <kg> [player_id] [restaurant_id]

Orders the given weight of every ingredient the restaurant's menu uses, and
nothing else, one order per category from the usual supplier, as the order
screen does: the supplier sets the price and the quality, and the player
pays. Never more than the restaurant's stock limit. Fails, buying nothing
more, at the first order the player can't afford.

Examples:
  restaurant stock menu 5         # 5kg of each ingredient on the menu
)",
      .args =
          {
              ArgSpec{
                  .name = "kg",
                  .type = ArgType::Int,
                  .optional = false,
                  .default_provider = DefaultProvider::None,
                  .description = "Kilograms of each ingredient",
                  .min_value = 1,
                  .max_value = max_ingredient_stock_kg,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const auto kg = static_cast<int>(ctx.int_arg(0));
        auto player_id = ctx.player_id(1);
        auto restaurant_id = ctx.restaurant_id(2);
        auto* player = ctx.state.player(player_id);
        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!player || !restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }
        const auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }
        std::set<core::food::IngredientType> used;
        for (const auto& entry : restaurant->menu().entries()) {
          if (const auto* recipe = player->recipes().find(entry.recipe_id)) {
            for (const auto& [type, grams] :
                 Restaurant::get_recipe_requirements_grams(*recipe)) {
              if (grams > 0) {
                used.insert(type);
              }
            }
          }
        }
        if (used.empty()) {
          return CommandResult::error(compat::format(
              "Restaurant {} has nothing on its menu", restaurant_id.value));
        }
        const auto r = restock_from_usual_suppliers(ctx.state, *player,
                                                    *restaurant, used, kg);
        if (!r.error.empty()) {
          return CommandResult::error(r.error);
        }
        return CommandResult::success().add_message(compat::format(
            "Bought up to {}kg of each of {} menu ingredients for ${:n} for "
            "restaurant {}",
            kg, r.ingredients, r.spent, restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_stock_clear() {
  return CommandDef{
      .name = "clear",
      .description = "Clear all ingredient stocks",
      .help_text =
          R"(Usage: restaurant stock clear [player_id] [restaurant_id]

Removes all ingredient stocks from the restaurant.

Examples:
  restaurant stock clear             # Clear active restaurant
  restaurant stock clear @ 0         # Clear restaurant 0 of active player
)",
      .args = {active_player_arg(), active_restaurant_arg()},
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto player_id = ctx.player_id(0);
        auto restaurant_id = ctx.restaurant_id(1);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        // Clear all stocks by setting quantity to 0
        for (int i = 0; i < 90; i++) {
          auto ingredient_type = static_cast<core::food::IngredientType>(i);
          auto& stock = restaurant->inventory().get_stock(ingredient_type);
          stock.quantity = 0;
          stock.weighted_avg_quality = 100;
          stock.weighted_avg_cost = Money{0};
        }

        return CommandResult::success(
            {"Cleared all ingredient stocks from restaurant"});
      },
  };
}

static CommandDef make_restaurant_contract() {
  return CommandDef{
      .name = "contract",
      .description = "Sign a supply contract with a supplier",
      .help_text =
          R"(Usage: restaurant contract <supplier_id> <ingredient_id> <kg_per_day> [player_id] [restaurant_id]

Signs a contract on the terms the supplier opens with, which is what the
player gets by accepting the first offer without haggling. The daily sum
comes from the supplier's own price, and the relationship is updated the way
signing in the conversation updates it, so deliveries and their penalties
behave as they would in play. Suppliers turn down an order below their
minimum, here as in the conversation.

Arguments:
  supplier_id   - Supplier to buy from; `city suppliers` lists them
  ingredient_id - Ingredient ID (0-21, 24-40, 48-60, 72-84)
  kg_per_day    - Kilograms delivered each day
  player_id     - Player ID (@ for active player)
  restaurant_id - Restaurant ID (@ for active restaurant)

Examples:
  restaurant contract 1 0 5      # 5kg of ingredient 0 a day from supplier 1
  restaurant contract 7 25 20    # 20kg of ingredient 25 a day from supplier 7
)",
      .args =
          {
              ArgSpec{
                  .name = "supplier_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Supplier id, from `city suppliers`",
                  .min_value = 0,
                  .max_value = 255,
              },
              ArgSpec{
                  .name = "ingredient_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Ingredient ID (0-89)",
                  .min_value = 0,
                  .max_value = 89,
              },
              ArgSpec{
                  .name = "kg_per_day",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Kilograms delivered each day",
                  .min_value = 1,
                  .max_value = 650,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto wanted_supplier = static_cast<uint8_t>(ctx.int_arg(0));
        auto ingredient_id = ctx.int_arg(1);
        auto kg_per_day = static_cast<int>(ctx.int_arg(2));
        auto player_id = ctx.player_id(3);
        auto restaurant_id = ctx.restaurant_id(4);

        auto* player = ctx.state.player(player_id);
        auto* restaurant = ctx.state.restaurant(restaurant_id);

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        auto category = core::food::CategoryRegistry::from_ingredient_id(
            static_cast<int>(ingredient_id));
        if (!category) {
          return CommandResult::error(
              compat::format("{} is not an ingredient id", ingredient_id));
        }
        auto ingredient_type =
            static_cast<core::food::IngredientType>(ingredient_id);

        const auto& city = restaurant->property().city();
        const FoodSupplier* supplier = nullptr;
        for (const auto& s : city.suppliers()) {
          if (s->category() == *category && s->id() == wanted_supplier) {
            supplier = s.get();
            break;
          }
        }
        if (!supplier) {
          return CommandResult::error(
              compat::format("No supplier {} for that category in {}",
                             wanted_supplier, city.name()));
        }
        if (!supplier->sells(ingredient_type)) {
          return CommandResult::error(
              compat::format("{} does not sell ingredient {}", supplier->name(),
                             ingredient_id));
        }

        auto price_per_kg = supplier->calculate_price(
            ingredient_type, city.ingredient_base_price(ingredient_type),
            FoodSupplier::price_multiplier(ctx.state.difficulty_level()));
        if (!price_per_kg) {
          return CommandResult::error(
              compat::format("{} has no price for ingredient {}",
                             supplier->name(), ingredient_id));
        }

        const Money daily_value{price_per_kg->cents * kg_per_day};
        if (daily_value < supplier->minimum_contract_amount()) {
          return CommandResult::error(compat::format(
              "{} contracts start at ${:n} a day; {}kg comes to ${:n}",
              supplier->name(), supplier->minimum_contract_amount(), kg_per_day,
              daily_value));
        }

        auto& rel = supplier->relationship(player_id);
        const SupplierContract terms = opening_contract_terms(
            *supplier, rel, supplier->spending_rank(player_id), daily_value);

        sign_contract(*player, *supplier, rel, terms, daily_value, city.id(),
                      restaurant->id(), {{ingredient_type, kg_per_day}});

        return CommandResult::success().add_message(compat::format(
            "Signed with {} for {}kg of ingredient {} a day, ${:n} daily at "
            "{}% off, {} days",
            supplier->name(), kg_per_day, ingredient_id, terms.total_value,
            terms.discount_score, terms.delivery_days));
      },
  };
}

static CommandDef make_restaurant_stock() {
  return CommandDef{
      .name = "stock",
      .description = "Manage restaurant ingredient stocks",
      .help_text = "Subcommands: buy, fill, ingredient, menu, clear",
  }
      .add_subcommand(make_restaurant_stock_buy())
      .add_subcommand(make_restaurant_stock_fill())
      .add_subcommand(make_restaurant_stock_ingredient())
      .add_subcommand(make_restaurant_stock_menu())
      .add_subcommand(make_restaurant_stock_clear());
}

static CommandDef make_restaurant_create() {
  return CommandDef{
      .name = "create",
      .description = "Create a new restaurant with layout",
      .help_text =
          R"(Usage: restaurant create <city_id> <player_id> <layout_id>

Creates a new restaurant for the specified player in the given city with a
specific layout. The restaurant will be placed in a random available property.

Arguments:
  city_id    - City ID (@ for active city, 0-9)
  player_id  - Player ID (@ for active player, 0-7)
  layout_id  - Layout number (1,3,5,7 are big; 2,4,6,8 are small)

Examples:
  restaurant create @ @ 1      # Active city/player, layout 1
  restaurant create 0 0 2      # City 0, player 0, layout 2
  restaurant create 6 1 5      # Madrid, player 1, layout 5
)",
      .args =
          {
              active_city_arg(),
              active_player_arg(),
              ArgSpec{
                  .name = "layout_id",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Layout ID (1-8)",
                  .min_value = 1,
                  .max_value = 8,
              },
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto city_id = ctx.city_id(0);
        auto player_id = ctx.player_id(1);
        auto layout_id = ctx.int_arg(2);

        // Ensure city is loaded
        ctx.state.ensure_city_loaded(city_id);
        auto* city = ctx.state.city(city_id);
        if (!city) {
          return CommandResult::error(
              compat::format("Failed to load city {}", city_id.value));
        }

        // Get player
        auto* player = ctx.state.player(player_id);
        if (!player) {
          return CommandResult::error(
              compat::format("Player {} does not exist", player_id.value));
        }

        // Create the restaurant (will find a random available property)
        auto* restaurant =
            ctx.state.real_estate().create_restaurant_with_layout(
                *player, *city, static_cast<int>(layout_id));

        if (!restaurant) {
          return CommandResult::error(compat::format(
              "Failed to create restaurant with layout {} in city {} (no "
              "available properties or layout not found)",
              layout_id, city_id.value));
        }

        return CommandResult::success().add_message(compat::format(
            "Created restaurant (ID {}) for player {} in "
            "city {} with layout {}",
            restaurant->id().value, player_id.value, city_id.value, layout_id));
      },
  };
}

static CommandDef make_restaurant_qs() {
  return CommandDef{
      .name = "qs",
      .description = "Quick start: Create fully functional restaurant",
      .help_text =
          R"(Usage: restaurant qs <city_id> <player_id>

Creates a fully functional small restaurant with:
- Layout 1 (small restaurant)
- All standard recipes loaded and added to menu at $8
- One chef hired
- One waiter hired
- All ingredients stocked with 500kg at quality 100
- Restaurant opened and ready to operate

This is useful for quickly setting up restaurants during development.

Arguments:
  city_id    - City ID (@ for active city, 0-9)
  player_id  - Player ID (@ for active player, 0-7)

Examples:
  restaurant qs @ @       # Active city/player
  restaurant qs 0 0       # City 0, player 0
  restaurant qs 3 1       # Paris, player 1
)",
      .args =
          {
              active_city_arg(),
              active_player_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        auto city_id = ctx.city_id(0);
        auto player_id = ctx.player_id(1);

        CommandResult result = CommandResult::success();

        // Ensure city is loaded
        ctx.state.ensure_city_loaded(city_id);
        auto* city = ctx.state.city(city_id);
        if (!city) {
          return CommandResult::error(
              compat::format("Failed to load city {}", city_id.value));
        }

        // Get player
        auto* player = ctx.state.player(player_id);
        if (!player) {
          return CommandResult::error(
              compat::format("Player {} does not exist", player_id.value));
        }

        // Ensure there is a valid active city
        if (!is_valid_city_id(ctx.state.active_city_id())) {
          ctx.state.active_city(city_id);
        }

        // Create small restaurant with layout 1
        auto* restaurant =
            ctx.state.real_estate().create_restaurant_with_layout(*player,
                                                                  *city, 1);

        if (!restaurant) {
          return CommandResult::error(compat::format(
              "Failed to create restaurant in city {} (no available "
              "properties or layout not found)",
              city_id.value));
        }

        result.add_message(compat::format(
            "Created small restaurant (ID {}) for player {} in city {}",
            restaurant->id().value, player_id.value, city_id.value));

        // So a following command can reach it as '@'.
        ctx.state.active_restaurant(restaurant->id());

        // Load standard recipes for the player
        auto& recipes = player->recipes();
        int loaded = 0;

        auto get_bob_size = core::food::topping_size_fn(*ctx.context.bob);

        for (int i = 0; i < core::food::STANDARD_PIZZA_COUNT; ++i) {
          const auto cid = static_cast<core::food::StandardPizza>(i);
          auto pizza = PizzaFactory::create_pizza(
              cid, get_bob_size, ctx.state.customer_taste_profiles());

          if (!pizza) {
            continue;
          }

          auto rid = recipes.create_standard(cid, std::move(*pizza));
          loaded++;

          // Add to menu at $8
          restaurant->menu().add_or_set(rid, Money{800});
        }

        result.add_message(compat::format(
            "Loaded {} standard recipes and added to menu at $8", loaded));

        // Hire a chef (using character ID 0)
        auto* chef_data = CharacterFactory::create_character(0);
        core::character::Character chef_character(chef_data);
        Staff chef(core::character::JobRole::CHEF, chef_character);
        chef.payment(
            chef_data->starting_payment(core::character::JobRole::CHEF));

        HiringResult chef_result = restaurant->add_staff(chef);
        if (chef_result == HiringResult::Success) {
          result.add_message(compat::format("Hired chef: {} {}",
                                            chef_data->first_name,
                                            chef_data->last_name));
        }
        else {
          result.add_message("Failed to hire chef");
        }

        // Hire a waiter (using character ID 1)
        auto* waiter_data = CharacterFactory::create_character(1);
        core::character::Character waiter_character(waiter_data);
        Staff waiter(core::character::JobRole::WAITER, waiter_character);
        waiter.payment(
            waiter_data->starting_payment(core::character::JobRole::WAITER));

        HiringResult waiter_result = restaurant->add_staff(waiter);
        if (waiter_result == HiringResult::Success) {
          result.add_message(compat::format("Hired waiter: {} {}",
                                            waiter_data->first_name,
                                            waiter_data->last_name));
        }
        else {
          result.add_message("Failed to hire waiter");
        }

        constexpr int quickstart_stock_kg = 500;
        const auto& stock_city = restaurant->property().city();
        Money stock_spent{0};
        for (auto category : ALL_INGREDIENT_CATEGORIES) {
          const auto* supplier = restaurant->supplier_for(stock_city, category);
          if (!supplier) {
            continue;
          }
          for (auto type : core::food::CategoryRegistry::range(category)) {
            if (auto cost =
                    buy_ingredient(ctx.state, *player, *restaurant, *supplier,
                                   type, quickstart_stock_kg)) {
              stock_spent = stock_spent + *cost;
            }
          }
        }
        result.add_message(
            compat::format("Bought {}kg of each ingredient for ${:n}",
                           quickstart_stock_kg, stock_spent));

        // Open the restaurant
        if (restaurant->is_force_closed()) {
          result.add_message("Restaurant is force-closed, cannot open");
        }
        else {
          restaurant->open_for_business(ctx.state);
          result.add_message("Restaurant opened successfully");
        }

        return result;
      },
  };
}

static CommandDef make_restaurant_inspection_timer() {
  return CommandDef{
      .name = "inspection-timer",
      .description = "Set the mayor inspection countdown on a restaurant",
      .help_text =
          R"(Usage: restaurant inspection-timer <days> [player_id] [restaurant_id]

Sets the number of days until the next scheduled mayor inspection.
The inspection fires naturally when the counter reaches zero at end-of-day.
Use 'actor mayor inspect' to trigger one immediately without waiting.

Examples:
  restaurant inspection-timer 1      # Inspection fires next end-of-day
  restaurant inspection-timer 0 @ 2  # Restaurant 2, fires immediately next EOD
)",
      .args =
          {
              ArgSpec{
                  .name = "days",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Days until next inspection (0 = next EOD)",
                  .min_value = 0,
                  .max_value = 365,
              },
              active_player_arg(),
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        int days = static_cast<int>(ctx.int_arg(0));
        auto player_id = ctx.player_id(1);
        auto restaurant_id = ctx.restaurant_id(2);

        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }

        auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player_id) {
          return CommandResult::error(
              compat::format("Restaurant {} is not owned by player {}",
                             restaurant_id.value, player_id.value));
        }

        restaurant->inspection_days_remaining(days);
        return CommandResult::success().add_message(compat::format(
            "Set inspection timer to {} day(s) for restaurant {}", days,
            restaurant_id.value));
      },
  };
}

static CommandDef make_restaurant_place() {
  return CommandDef{
      .name = "place",
      .description = "Buy one piece of furniture and place it",
      .help_text =
          R"(Usage: restaurant place <furniture> <x> <y> [rotation] [restaurant_id]

Buys one piece at its shop price and places it the way the placement mode
does: kitchenware goes in the kitchen, everything else in the dining room;
walls and other furniture block it, chairs may overlap tables and face the
nearest one. x and y are the piece's top-left in pixels from the room's
first interior pixel (one tile is 24 x 22 pixels).

Arguments:
  furniture     - Furniture name as in furniture.json, e.g. CAPPUCCINO
  x, y          - Top-left inside the room, in pixels
  rotation      - south, west, north or east (chairs and tables)
  restaurant_id - Restaurant ID (@ for active restaurant)

Examples:
  restaurant place CAPPUCCINO 48 44         # a table
  restaurant place WOODEN_CHAIR 30 44 east  # a chair facing east
  restaurant place OVEN_SMALL 0 0           # an oven in the kitchen
)",
      .args =
          {
              ArgSpec{
                  .name = "furniture",
                  .type = ArgType::String,
                  .optional = false,
                  .default_provider = DefaultProvider::None,
                  .description = "Furniture name",
              },
              ArgSpec{
                  .name = "x",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Pixels from the left interior edge",
                  .min_value = 0,
                  .max_value = 2000,
              },
              ArgSpec{
                  .name = "y",
                  .type = ArgType::Int,
                  .optional = false,
                  .description = "Pixels from the top interior edge",
                  .min_value = 0,
                  .max_value = 2000,
              },
              ArgSpec{
                  .name = "rotation",
                  .type = ArgType::String,
                  .optional = true,
                  .default_provider = DefaultProvider::None,
                  .description = "south, west, north or east",
              },
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const auto type =
            magic_enum::enum_cast<FurnitureType>(ctx.string_arg(0));
        if (!type) {
          return CommandResult::error(
              compat::format("No furniture called {}", ctx.string_arg(0)));
        }
        const auto* furniture = FurnitureFactory::create_furniture(*type);
        if (!furniture) {
          return CommandResult::error("That furniture cannot be bought");
        }
        auto* restaurant = ctx.state.restaurant(ctx.restaurant_id(4));
        auto* player = ctx.state.active_player();
        if (!restaurant || !player) {
          return CommandResult::error("No such restaurant");
        }
        const auto owner = restaurant->property().owner_or_tenant();
        if (!owner || *owner != player->id()) {
          return CommandResult::error(
              "The restaurant is not the active player's");
        }
        if (!ctx.context.bob) {
          return CommandResult::error("No sprites loaded to measure with");
        }
        if (player->cash_balance() < furniture->price()) {
          return CommandResult::error(
              compat::format("{} costs {:w}, more than the player has",
                             furniture->name(), furniture->price()));
        }

        auto piece = std::make_unique<PlacedFurniture>(
            furniture, restaurant->layout()->next_furniture_id(),
            WorldPoint{0, 0}, core::FurnitureId{UNLINKED_FURNITURE}, 0);
        if (ctx.has_arg(3)) {
          const std::string r = ctx.string_arg(3);
          if (r == "south") {
            piece->rotate(Rotation::SOUTH);
          }
          else if (r == "west") {
            piece->rotate(Rotation::WEST);
          }
          else if (r == "north") {
            piece->rotate(Rotation::NORTH);
          }
          else if (r == "east") {
            piece->rotate(Rotation::EAST);
          }
          else {
            return CommandResult::error(
                "Rotation is south, west, north or east");
          }
        }
        const auto origin = core::restaurant::interior_origin();
        const WorldPoint at{origin.x + static_cast<int>(ctx.int_arg(1)),
                            origin.y + static_cast<int>(ctx.int_arg(2))};
        const auto size = core::restaurant::furniture_size_fn(*ctx.context.bob);
        const auto id = piece->id();
        const bool kitchen = core::restaurant::goes_in_kitchen(*piece);
        switch (
            core::restaurant::place_furniture(*restaurant, piece, at, size)) {
        case core::restaurant::PlaceResult::Wall:
          return CommandResult::error("It does not fit inside the room there");
        case core::restaurant::PlaceResult::Blocked:
          return CommandResult::error("Other furniture is in the way");
        case core::restaurant::PlaceResult::Placed:
          break;
        }

        // Paid and timed like a shop order of one piece, then its placement.
        (void)ctx.state.spend_cash(kitchen ? MonthlyCategory::Machines
                                           : MonthlyCategory::Furniture,
                                   furniture->price(), restaurant->id());
        restaurant->purchase_furniture(furniture, 1);
        ctx.state.advance_time_blocks(3 + 1);
        restaurant->recalculate_economy_metrics(ctx.state);
        return CommandResult::success().add_message(compat::format(
            "Placed {} (furniture {}) in the {} at {},{}", furniture->name(),
            id.value, kitchen ? "kitchen" : "dining room", ctx.int_arg(1),
            ctx.int_arg(2)));
      },
  };
}

static CommandDef make_restaurant_menu() {
  return CommandDef{
      .name = "menu",
      .description = "Put the owner's standard pizzas on the menu",
      .help_text = R"(Usage: restaurant menu <dollars> [restaurant_id]

Puts every standard pizza the restaurant's owner has a recipe for on the
restaurant's menu at one price, in whole dollars, as setting them one by one
in the menu screen does. Pizzas already on the menu get the new price.

Examples:
  restaurant menu 8          # The active restaurant, $8 each
  restaurant menu 10 2       # Restaurant 2, $10 each
)",
      .args =
          {
              ArgSpec{
                  .name = "dollars",
                  .type = ArgType::Int,
                  .optional = false,
                  .default_provider = DefaultProvider::None,
                  .description = "Price of each pizza in dollars",
                  .min_value = 1,
                  .max_value = 99,
              },
              active_restaurant_arg(),
          },
      .handler = [](const CommandContext& ctx) -> CommandResult {
        const auto dollars = ctx.int_arg(0);
        const auto restaurant_id = ctx.restaurant_id(1);
        auto* restaurant = ctx.state.restaurant(restaurant_id);
        if (!restaurant) {
          return CommandResult::error(
              compat::format("Restaurant {} not found", restaurant_id.value));
        }
        const auto owner = restaurant->property().owner_or_tenant();
        auto* player = owner ? ctx.state.player(*owner) : nullptr;
        if (!player) {
          return CommandResult::error(compat::format(
              "Restaurant {} has no owner", restaurant_id.value));
        }
        int on_menu = 0;
        for (const auto& slot : player->recipes().standard) {
          if (slot.created()) {
            restaurant->menu().add_or_set(slot.recipe->id(),
                                          Money::from_dollars(dollars));
            ++on_menu;
          }
        }
        if (on_menu == 0) {
          return CommandResult::error(compat::format(
              "Player {} has no standard recipes", owner->value));
        }
        return CommandResult::success().add_message(
            compat::format("{} standard pizzas on restaurant {}'s menu at ${}",
                           on_menu, restaurant_id.value, dollars));
      },
  };
}

CommandDef make_restaurant_commands() {
  return CommandDef{
      .name = "restaurant",
      .aliases = {"r", "rest"},
      .description = "Restaurant management commands",
      .help_text =
          "Manage restaurants: info, clear, open, close, shutdown, ai-layout, "
          "furniture, stock, create, qs, place, menu, inspection-timer",
  }
      .add_subcommand(make_restaurant_info())
      .add_subcommand(make_restaurant_clear())
      .add_subcommand(make_restaurant_open())
      .add_subcommand(make_restaurant_close())
      .add_subcommand(make_restaurant_shutdown())
      .add_subcommand(make_restaurant_ai_layout())
      .add_subcommand(make_restaurant_furniture())
      .add_subcommand(make_restaurant_stock())
      .add_subcommand(make_restaurant_contract())
      .add_subcommand(make_restaurant_create())
      .add_subcommand(make_restaurant_qs())
      .add_subcommand(make_restaurant_place())
      .add_subcommand(make_restaurant_menu())
      .add_subcommand(make_restaurant_inspection_timer());
}

} // namespace debug::commands
