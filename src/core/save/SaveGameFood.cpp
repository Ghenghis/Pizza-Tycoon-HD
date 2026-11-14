/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/SaveGameFood.h"

#include <utility>

#include "core/food/IngredientType.h"
#include "core/food/StandardPizza.h"
#include "core/save/SaveGameEnums.h"
#include "engine/geom/Spaces.h"

namespace save {

// Every enum a pizza's save holds, as the checks below expect it.
static_assert(enum_runs(core::food::Origin::Standard,
                        core::food::Origin::User));
// COUNT ends the list and is no pizza of its own.
static_assert(enum_runs(core::food::StandardPizza::PizzaNapoletana,
                        core::food::StandardPizza::COUNT));

json topping_to_json(const core::food::Topping& topping) {
  json j;
  j["ingredient_id"] = static_cast<int>(topping.ingredient_type());
  j["state"] = topping.state_raw();
  j["origin"] = {{"x", topping.origin().x}, {"y", topping.origin().y}};
  return j;
}

std::unique_ptr<core::food::Topping> json_to_topping(const json& j) {
  // An ingredient the game doesn't have ends the game when the topping is
  // built, so it is checked here.
  const auto ingredient = checked_enum<core::food::IngredientType>(
      j.at("ingredient_id"), "a topping's ingredient");
  const int ingredient_id = static_cast<int>(ingredient);
  int state = j.at("state").get<int>();
  WorldPoint origin{j.at("origin").at("x").get<int>(),
                    j.at("origin").at("y").get<int>()};

  return std::make_unique<core::food::Topping>(ingredient_id, state, origin);
}

json pizza_to_json(const core::food::Pizza& pizza) {
  using namespace core::food;

  json j;
  j["name"] = pizza.name();
  j["finalized"] = pizza.is_finalized();
  j["origin"] = static_cast<int>(pizza.origin());

  // Save toppings
  json toppings_array = json::array();
  for (const auto& topping : pizza.toppings()) {
    toppings_array.push_back(topping_to_json(*topping));
  }
  j["toppings"] = std::move(toppings_array);

  // Save scores (5 judges)
  j["scores"] = pizza.scores();

  // Save popularity (6 demographic groups)
  json popularity_array = json::array();
  auto pop = pizza.popularity();
  for (size_t i = 0; i < static_cast<size_t>(ConsumerType::Count); ++i) {
    popularity_array.push_back(pop.values[i]);
  }
  j["popularity"] = std::move(popularity_array);

  // Save flavors
  auto flavors = pizza.flavors();
  j["flavors"] = {{"fat", flavors.fat},
                  {"sweet", flavors.sweet},
                  {"salty", flavors.salty},
                  {"hot", flavors.hot}};

  // Save stats
  j["calories"] = pizza.calories();
  j["appearance"] = pizza.appearance();
  j["refinement"] = pizza.refinement();
  j["normality"] = pizza.normality();
  j["imitation"] = pizza.imitation();

  return j;
}

core::food::Pizza
json_to_pizza(const json& j,
              const core::food::CustomerTasteProfiles& taste_profiles) {
  using namespace core::food;

  std::string name = j.at("name").get<std::string>();
  auto pizza = Pizza(name);

  // Restore origin
  pizza.set_origin(checked_enum<Origin>(j.at("origin"), "a pizza's origin"));

  // Restore toppings
  if (j.contains("toppings")) {
    for (const auto& topping_json : j.at("toppings")) {
      pizza.add_topping(json_to_topping(topping_json));
    }
  }

  // Finalize if it was finalized when saved
  if (j.at("finalized").get<bool>()) {
    pizza.finalize(taste_profiles, name);
  }

  // Finalize recalculates these from the toppings, so restore the saved values
  // afterwards.
  if (j.contains("flavors")) {
    Flavors flavors;
    flavors.fat = j.at("flavors").at("fat").get<int>();
    flavors.sweet = j.at("flavors").at("sweet").get<int>();
    flavors.salty = j.at("flavors").at("salty").get<int>();
    flavors.hot = j.at("flavors").at("hot").get<int>();

    int appearance = j.at("appearance").get<int>();
    int refinement = j.at("refinement").get<int>();
    int normality = j.at("normality").get<int>();
    int imitation = j.value("imitation", Pizza::full_imitation);

    pizza.set_disk_stats(flavors, appearance, refinement, normality, imitation,
                         taste_profiles);
  }

  return pizza;
}

json recipe_to_json(const core::food::Recipe& recipe) {
  using namespace core::food;

  json j;
  j["id"] = recipe.id().value;
  j["origin"] = static_cast<int>(recipe.origin());
  j["display_name"] = recipe.display_name();

  if (recipe.reference_id().has_value()) {
    j["reference_id"] = static_cast<int>(*recipe.reference_id());
  }
  else {
    j["reference_id"] = nullptr;
  }

  // Save the pizza
  j["pizza"] = pizza_to_json(recipe.pizza());

  j["daily_sales"] = recipe.daily_sales();

  return j;
}

core::food::Recipe
json_to_recipe(const json& j,
               const core::food::CustomerTasteProfiles& taste_profiles) {
  using namespace core::food;

  core::RecipeId id{j.at("id").get<int>()};
  std::string display_name = j.at("display_name").get<std::string>();
  Pizza pizza = json_to_pizza(j.at("pizza"), taste_profiles);

  auto recipe = [&] {
    const json& reference = j.contains("reference_id") ? j.at("reference_id")
                                                       : j.at("canonical_id");
    if (!reference.is_null()) {
      auto reference_id =
          checked_enum(reference, StandardPizza::PizzaNapoletana,
                       StandardPizza::VegetablePizza, "a standard pizza");
      return Recipe::standard(id, reference_id, std::move(display_name),
                              std::move(pizza));
    }
    return Recipe::user(id, std::move(display_name), std::move(pizza));
  }();

  if (j.contains("daily_sales")) {
    recipe.daily_sales(j.at("daily_sales").get<Recipe::DailySales>());
  }
  return recipe;
}

} // namespace save
