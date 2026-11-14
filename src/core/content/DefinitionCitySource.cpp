/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/DefinitionCitySource.h"

#include <array>
#include <cassert>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>

#include "core/character/Actor.h"
#include "core/city/City.h"
#include "core/city/District.h"
#include "core/city/Property.h"
#include "core/content/Roster.h"
#include "core/factories/CityFactory.h"
#include "core/finance/Money.h"
#include "core/settings/BankSettings.h"
#include "core/settings/InsuranceSettings.h"
#include "core/settings/MafiaSettings.h"
#include "core/settings/RealtorSettings.h"
#include "core/supply/FoodSupplier.h"
#include "engine/Fatal.h"

namespace core::content {

namespace {

constexpr int names_per_type = 24;

DistrictType district_type(DistrictKind kind) {
  return static_cast<DistrictType>(static_cast<int>(kind));
}

std::unique_ptr<District> make_district(int index,
                                        const DistrictDefinition& d) {
  auto district = std::make_unique<District>(index, d.name);
  // The same setters, in the same order, as CityFactory::load_districts.
  district->id(static_cast<uint8_t>(d.number));
  district->control_screen_alternate_color(d.control_screen_alternate_color);
  district->traffic_density(d.traffic_intensity);
  district->average_income_factor(d.average_income_factor);
  district->crime_statistics(d.crime_stats);
  district->district_type(district_type(d.kind));
  district->service_quality_factor(
      static_cast<uint8_t>(d.service_speed_sensitivity));
  district->minimum_desired_standard_pizza_count(
      static_cast<uint8_t>(d.min_standard_pizzas));
  district->minimum_desired_user_pizza_count(
      static_cast<uint8_t>(d.min_user_pizzas));
  district->color(CityFactory::district_color(static_cast<std::size_t>(index)));
  district->average_size(d.average_premise_size);
  district->average_size_modifier(d.average_size_modifier);
  district->price_level(d.price_level);
  district->rent_level(d.rent_level);
  district->premises_availability(d.premises_availability);
  district->demographic_factor(static_cast<uint8_t>(d.demographic_factor));
  district->demographic_base(static_cast<uint8_t>(d.demographic_base));

  constexpr std::size_t groups = population_groups;
  std::array<float, groups> demand{};
  float sum = 0.0f;
  for (std::size_t c = 0; c < groups; ++c) {
    demand[c] = static_cast<float>(d.population_weights_milli[c]) / 1000.0f;
    sum += demand[c];
  }
  district->demographic_demand(demand);
  std::array<uint8_t, groups> percentages{};
  for (std::size_t c = 0; c < groups; ++c) {
    const float percent = sum == 0.0f ? 0.0f : (demand[c] / sum) * 100.0f;
    percentages[c] = static_cast<uint8_t>(std::floor(percent));
  }
  district->population_percentages(percentages);
  return district;
}

// Named-building tiles take the next name of their type in map order,
// wrapping exactly as CityFactory::load_map_data does.
std::map<int, std::string>
named_buildings(const LocationDefinition& def,
                const std::array<std::array<std::string, names_per_type>,
                                 named_building_types>& names,
                const std::array<int, named_building_types>& counts) {
  std::map<int, std::string> out;
  std::array<uint8_t, named_building_types> used{};
  for (int i = 0; i < grid_tiles; ++i) {
    const auto c = def.tiles[static_cast<std::size_t>(i)];
    if (!City::is_named_building(c)) {
      continue;
    }
    const auto type = static_cast<std::size_t>(((c & 0xF0) >> 4) - 8);
    int index = used[type];
    if (++used[type] > static_cast<uint8_t>(counts[type])) {
      index = used[type] = 0;
    }
    out[i] = names[type][static_cast<std::size_t>(index)];
  }
  return out;
}

struct ActorIds {
  std::unordered_map<std::string, core::ActorId> by_entity;
  core::ActorId next{0};
};

void add_actors(City& city, const LocationDefinition& def) {
  std::map<core::ActorId, std::unique_ptr<Actor>> actors;
  std::unordered_map<core::ActorId, BankSettings> banks;
  std::unordered_map<core::ActorId, InsuranceSettings> insurers;
  std::unordered_map<core::ActorId, MafiaSettings> underworld;
  std::unordered_map<core::ActorId, RealtorSettings> realtors;
  core::ActorId next{0};

  const auto add = [&](const ActorDefinition& a, ActorType type) {
    const core::ActorId id = next;
    auto actor = std::make_unique<Actor>(type, id);
    actor->phone_number(a.phone);
    actor->name(a.name);
    actor->head_id(a.head);
    actors[id] = std::move(actor);
    next++;
    return id;
  };

  // Classic id order: realtors, banks, insurers, mayor, police, mafia, arms
  // dealers (CityFactory::build_city_object).
  const auto& list = def.actors;
  for (const auto& a : list.realtors) {
    const auto id = add(a, ActorType::REALTOR);
    realtors.emplace(id,
                     RealtorSettings(static_cast<uint8_t>(a.unknown_1),
                                     static_cast<uint8_t>(a.unknown_2),
                                     static_cast<uint8_t>(a.rip_off_chance)));
  }
  for (const auto& a : list.banks) {
    const auto id = add(a, ActorType::BANK);
    banks.emplace(id,
                  BankSettings(static_cast<uint8_t>(a.interest_rate_modifier),
                               static_cast<uint8_t>(a.loan_amount_multiplier)));
  }
  for (const auto& a : list.insurers) {
    const auto id = add(a, ActorType::INSURANCE);
    insurers.emplace(
        id, InsuranceSettings(static_cast<uint8_t>(a.premium_factor),
                              static_cast<uint8_t>(a.willingness_threshold),
                              static_cast<uint8_t>(a.rejection_percentage)));
  }
  assert(list.mayor.has_value());
  const auto mayor_id = add(*list.mayor, ActorType::MAYOR);
  std::optional<core::ActorId> police_id;
  for (const auto& a : list.police) {
    const auto id = add(a, ActorType::POLICE);
    if (!police_id) {
      police_id = id;
    }
  }
  const auto add_underworld = [&](const ActorDefinition& a, ActorType type) {
    const auto id = add(a, type);
    underworld.emplace(id,
                       MafiaSettings(static_cast<uint8_t>(a.crime_danger_level),
                                     static_cast<uint8_t>(a.unknown_1),
                                     static_cast<uint8_t>(a.unknown_2),
                                     static_cast<uint8_t>(a.unknown_3)));
  };
  for (const auto& a : list.mafia) {
    add_underworld(a, ActorType::MAFIA);
  }
  for (const auto& a : list.arms_dealers) {
    add_underworld(a, ActorType::ARMS_DEALER);
  }

  assert(police_id.has_value());
  city.police_id(*police_id);
  // Named explicitly: the Classic loader never sets it (section 18, item 1).
  city.mayor_id(mayor_id);
  city.actors(std::move(actors));
  city.bank_settings(std::move(banks));
  city.insurance_settings(std::move(insurers));
  city.mafia_settings(std::move(underworld));
  city.realtor_settings(std::move(realtors));
}

void add_properties(City& city, const LocationDefinition& def) {
  // Realtors take ActorIds 0..n-1, in their listed order.
  std::unordered_map<std::string, int> realtor_ids;
  for (std::size_t i = 0; i < def.actors.realtors.size(); ++i) {
    realtor_ids.emplace(def.actors.realtors[i].id, static_cast<int>(i));
  }
  std::map<int, std::unique_ptr<Property>> properties;
  for (const auto& p : def.properties) {
    const auto realtor = realtor_ids.find(p.realtor);
    assert(realtor != realtor_ids.end());
    auto property = std::make_unique<Property>(
        p.tile_index(), p.kind, Size{p.interior_w, p.interior_h},
        core::ActorId{realtor->second}, p.initial_status, city);
    property->purchase_price(Money{p.purchase_price_cents});
    property->kitchen_size(Size{p.kitchen_w, p.kitchen_h});
    property->entrance_offset(p.entrance_offset);
    property->kitchen_entry_offset(p.kitchen_entry_offset);
    properties[p.tile_index()] = std::move(property);
  }
  city.properties(std::move(properties));
}

void add_suppliers(City& city, const LocationDefinition& def) {
  std::vector<std::unique_ptr<FoodSupplier>> suppliers;
  suppliers.reserve(def.food_suppliers.size());
  for (const auto& s : def.food_suppliers) {
    auto supplier = std::make_unique<FoodSupplier>(
        s.category, static_cast<uint8_t>(s.number),
        static_cast<uint8_t>(s.quality), s.name, static_cast<uint8_t>(s.head),
        Money::from_dollars(s.minimum_contract_dollars), s.price_levels.data(),
        static_cast<int>(s.price_levels.size()));
    supplier->price_factor(static_cast<uint8_t>(s.price_factor));
    supplier->loyalty_level_amount(
        Money::from_dollars(s.loyalty_level_dollars));
    suppliers.push_back(std::move(supplier));
  }
  city.suppliers(std::move(suppliers));
}

// Everything a definition fixes, set on city without drawing any number.
void fill_static(City& city, const LocationDefinition& def) {
  std::array<std::array<std::string, names_per_type>, named_building_types>
      names{};
  std::array<int, named_building_types> counts{};
  for (std::size_t t = 0; t < named_building_types; ++t) {
    for (const auto& name : def.building_names[t]) {
      if (counts[t] < names_per_type) {
        names[t][static_cast<std::size_t>(counts[t]++)] = name;
      }
    }
  }

  // The order of CityFactory::build_city_object.
  city.data_length(grid_tiles);
  city.map_data(def.tiles);
  city.named_buildings(named_buildings(def, names, counts));
  city.building_names(names, counts);

  std::array<uint8_t, 256> types{};
  std::array<std::string, 10> type_names{};
  CityFactory::tile_types(types, type_names);
  city.type_names(type_names);

  std::vector<std::unique_ptr<District>> districts;
  for (std::size_t i = 0; i < def.districts.size(); ++i) {
    auto district = make_district(static_cast<int>(i), def.districts[i]);
    district->city(&city);
    districts.push_back(std::move(district));
  }
  city.districts(std::move(districts));
  city.types(types);
  std::array<uint8_t, cell_count> cells{};
  for (std::size_t i = 0; i < cells.size(); ++i) {
    cells[i] = def.district_cells[i];
  }
  city.district_bez(cells);

  add_properties(city, def);
  add_actors(city, def);
  add_suppliers(city, def);

  IngredientPrices prices{};
  for (std::size_t i = 0; i < ingredient_price_count; ++i) {
    prices[static_cast<int>(i)] = Money{def.ingredient_base_prices[i]};
  }
  city.ingredient_base_prices(prices);
}

} // namespace

std::unique_ptr<City> build_static_city(core::CityId id,
                                        const LocationDefinition& def) {
  auto city = std::make_unique<City>(id);
  city->name(def.name);
  fill_static(*city, def);
  return city;
}

std::unique_ptr<City>
build_city(core::CityId id, const LocationDefinition& def, bool fix_legacy_bugs,
           const std::vector<std::pair<std::uint8_t, int>>& player_chars) {
  // The order of CityFactory::create_city: the city, the mayor's account,
  // the static parts, the start-up, the job market. Only the draws of
  // populate_properties are missing, because the definition holds what they
  // would have drawn.
  auto city = std::make_unique<City>(id);
  city->name(def.name);
  city->mayor_office().init_private_account();
  fill_static(*city, def);
  CityFactory::initialise_new_city(*city, fix_legacy_bugs);
  city->job_market().init(player_chars);
  return city;
}

std::unique_ptr<City> build_roster_city(
    const Roster& roster, core::CityId id, bool fix_legacy_bugs,
    const std::vector<std::pair<std::uint8_t, int>>& player_chars) {
  const RosterEntry& entry = roster.entry(id);
  if (entry.source == RosterSource::Classic) {
    return CityFactory::create_city(id, *entry.classic_number, fix_legacy_bugs,
                                    player_chars);
  }
  if (!entry.definition) {
    fatal("The roster entry " + entry.key.str() + " has no definition");
  }
  return build_city(id, *entry.definition, fix_legacy_bugs, player_chars);
}

} // namespace core::content
