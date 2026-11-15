/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The restock commands of scripted runs: `restaurant stock menu <kg>` buys
// only what the menu's pizzas use and `restaurant stock ingredient <id> <kg>`
// one ingredient, both from the usual suppliers through the order screen's
// purchase (buy_for_restaurant).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

#include "ContentJourney.h"
#include "ContentTestGame.h"
#include "core/character/Player.h"
#include "core/food/IngredientType.h"
#include "core/food/PlayerRecipes.h"
#include "core/restaurant/Restaurant.h"

using content_journey::run;
using content_journey::try_run;

namespace {

int grams(const Restaurant& r, core::food::IngredientType t) {
  const auto* stock = r.inventory().get_ingredient_stock(t);
  return stock ? stock->quantity : 0;
}

} // namespace

TEST_CASE("Restock commands buy what the menu uses, and one ingredient",
          "[content][stock][game]") {
  CONTENT_REQUIRE_GAME_DATA();
  content_test::TestGame game;
  const auto j =
      content_journey::set_up_shop(game, content_journey::Fit::PieceByPiece);
  Restaurant& r = *j.trading;
  Player& me = *game.state.player(core::PlayerId{0});
  const std::string rid = std::to_string(r.id().value);

  std::set<core::food::IngredientType> used;
  for (const auto& entry : r.menu().entries()) {
    const auto* recipe = me.recipes().find(entry.recipe_id);
    REQUIRE(recipe != nullptr);
    for (const auto& [type, g] :
         Restaurant::get_recipe_requirements_grams(*recipe)) {
      if (g > 0) {
        used.insert(type);
      }
    }
  }
  REQUIRE_FALSE(used.empty());

  std::array<int, max_ingredient_count> before{};
  for (int i = 0; i < max_ingredient_count; ++i) {
    before[static_cast<std::size_t>(i)] =
        grams(r, static_cast<core::food::IngredientType>(i));
  }
  const Money cash = me.cash_balance();
  run(game, "restaurant stock menu 2 @ " + rid);
  CHECK(me.cash_balance() < cash);
  int bought = 0;
  for (int i = 0; i < max_ingredient_count; ++i) {
    const auto t = static_cast<core::food::IngredientType>(i);
    const int now = grams(r, t);
    INFO("ingredient " << i);
    if (used.contains(t)) {
      // 2 kg more of each, where the usual supplier sells it (`stock fill`
      // stocked exactly those).
      if (before[static_cast<std::size_t>(i)] > 0) {
        CHECK(now == before[static_cast<std::size_t>(i)] + 2000);
        ++bought;
      }
    }
    else {
      CHECK(now == before[static_cast<std::size_t>(i)]); // nothing else
    }
  }
  CHECK(bought > 0);

  // One ingredient: tomatoes, on every standard pizza.
  const auto tomato = core::food::IngredientType::TOMATOES;
  const int tomatoes = grams(r, tomato);
  const Money cash2 = me.cash_balance();
  run(game, "restaurant stock ingredient 0 1 @ " + rid);
  CHECK(grams(r, tomato) == tomatoes + 1000);
  CHECK(me.cash_balance() < cash2);

  // Not an ingredient.
  CHECK(try_run(game, "restaurant stock ingredient 22 1 @ " + rid).status !=
        debug::command::CommandStatus::Success);
}
