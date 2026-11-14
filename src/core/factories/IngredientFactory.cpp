/*
 * SPDX-FileCopyrightText: 2013 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/factories/IngredientFactory.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/factories/DataFile.h"
#include "core/factories/FillOnce.h"
#include "core/food/Ingredient.h"
#include "core/food/IngredientType.h"
#include "engine/Fatal.h"
#include "engine/FormatCompat.h"

using namespace core::food;

namespace {

std::map<IngredientType, IngredientData> ingredients_;

IngredientData read_ingredient(const data_file::json& entry,
                               IngredientType type) {
  return IngredientData{
      .type = type,
      .weight_large = entry.at("weight_large").get<int>(),
      .weight_medium = entry.at("weight_medium").get<int>(),
      .weight_small = entry.at("weight_small").get<int>(),
      .calories = entry.at("calories").get<int>(),
      .intensity = entry.at("intensity").get<int>(),
      .fat = entry.at("fat").get<int>(),
      .sweet = entry.at("sweet").get<int>(),
      .salty = entry.at("salty").get<int>(),
      .hot = entry.at("hot").get<int>(),
      .category = data_file::enum_field<IngredientCategory>(entry, "category"),
      .bobs = data_file::array_field<int, 12>(entry, "bobs"),
  };
}

FillOnce filled;

} // namespace

std::unordered_map<int, std::unique_ptr<Ingredient>>
    IngredientFactory::cache_{};

Ingredient* IngredientFactory::create_ingredient(int ingredient_id) {
  // Every ingredient at once, the first time, so a game being built on a
  // worker reads the same table the game does, without locks.
  filled.ensure([] {
    ingredients_ = data_file::load<IngredientData, IngredientType>(
        "ingredients.json", read_ingredient);
    for (const auto& [type, data] : ingredients_) {
      cache_.emplace(static_cast<int>(type),
                     std::make_unique<Ingredient>(data));
    }
  });
  const auto it = cache_.find(ingredient_id);
  if (it == cache_.end()) {
    fatal(compat::format("IngredientId {} is unknown.", ingredient_id));
  }
  return it->second.get();
}
