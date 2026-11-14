/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/city/CityTypes.h"
#include "core/content/ContentKey.h"
#include "core/food/IngredientCategoryType.h"
#include "core/util/Constants.h"

namespace core::content {

// A LocationDefinition as read from docs/studio/location.schema.json: every
// value a city needs, realized, so building the city from it draws no random
// numbers (location-contract.md sections 3 and 4). Read with read_location
// (LocationReader.h), checked with validate_location (LocationValidator.h);
// only a definition without errors may reach DefinitionCitySource.

inline constexpr int grid_width = MAP_TILES_PER_ROW;
inline constexpr int grid_height = MAP_TILES_PER_COLUMN;
inline constexpr int grid_tiles = grid_width * grid_height;
inline constexpr int cell_columns = NUMBER_OF_DISTRICT_CELL_COLUMNS;
inline constexpr int cell_rows = NUMBER_OF_DISTRICT_CELL_ROWS;
inline constexpr int cell_count = cell_columns * cell_rows;
inline constexpr int tiles_per_cell = 8;
inline constexpr std::size_t max_districts = 16;
inline constexpr std::size_t ingredient_price_count = 96;
inline constexpr std::size_t supplier_price_levels = 22;
inline constexpr std::size_t named_building_types = 8;
inline constexpr std::size_t population_groups = 6;

// The district types of dat/cityN.pc, in DistrictType order (District.h).
enum class DistrictKind {
  Residential = 1,
  Industrial,
  Business,
  Leisure,
  Shopping,
  Traffic,
};

enum class StreetSide {
  North,
  East,
  South,
  West,
};

enum class AttachmentState {
  Quarantine,
  Approved,
  Rejected,
};

struct DistrictDefinition {
  std::string id;
  std::string name;
  int number{1};
  DistrictKind kind{DistrictKind::Residential};
  int traffic_intensity{0};
  int crime_stats{0};
  bool control_screen_alternate_color{false};
  int demographic_factor{0};
  int demographic_base{0};
  int average_income_factor{0};
  int service_speed_sensitivity{0};
  // Stored decoded: the Classic loader divides by 10 and shifts by 4.
  int min_standard_pizzas{0};
  int min_user_pizzas{0};
  int average_premise_size{0};
  int average_size_modifier{0};
  int price_level{0};
  int rent_level{0};
  int premises_availability{0};
  // Thousandths: the runtime weight is value / 1000.
  std::array<int, population_groups> population_weights_milli{};
};

struct PropertyDefinition {
  std::string id;
  PropertyType kind{PropertyType::BUSINESS_PREMISE};
  int x{0};
  int y{0};
  StreetSide street_side{StreetSide::South};
  int interior_w{10};
  int interior_h{10};
  int kitchen_w{12};
  int kitchen_h{7};
  int entrance_offset{1};
  int kitchen_entry_offset{0};
  // Entity id of a realtor (REF-1).
  std::string realtor;
  PremiseStatus initial_status{PremiseStatus::NOT_AVAILABLE};
  std::int64_t purchase_price_cents{0};

  [[nodiscard]] int tile_index() const { return (y * grid_width) + x; }
};

// One contact. Fields that do not apply to the actor's role stay zero.
struct ActorDefinition {
  std::string id;
  std::string name;
  int head{1};
  std::uint32_t phone{0};
  // Realtors.
  int rip_off_chance{0};
  // Realtors and the underworld (unknown_3 is underworld only).
  int unknown_1{0};
  int unknown_2{0};
  int unknown_3{0};
  // Banks.
  int interest_rate_modifier{0};
  int loan_amount_multiplier{0};
  // Insurers.
  int premium_factor{0};
  int willingness_threshold{0};
  int rejection_percentage{0};
  // Mafia and arms dealers.
  int crime_danger_level{0};
};

struct ActorDefinitions {
  std::vector<ActorDefinition> realtors;
  std::vector<ActorDefinition> banks;
  std::vector<ActorDefinition> insurers;
  std::optional<ActorDefinition> mayor;
  std::vector<ActorDefinition> police;
  std::vector<ActorDefinition> mafia;
  std::vector<ActorDefinition> arms_dealers;
};

struct SupplierDefinition {
  std::string id;
  int number{0};
  IngredientCategory category{IngredientCategory::VEGETABLES};
  std::string name;
  int quality{0};
  int price_factor{0};
  int head{1};
  int minimum_contract_dollars{0};
  std::int64_t loyalty_level_dollars{0};
  // Per product within the category; zero means not sold.
  std::array<std::uint8_t, supplier_price_levels> price_levels{};
};

struct AttachmentDefinition {
  std::string id;
  std::string path;
  AttachmentState state{AttachmentState::Quarantine};
};

struct GeoPoint {
  std::int64_t lon_e6{0};
  std::int64_t lat_e6{0};
};

struct LocationDefinition {
  ContentKey key = *ContentKey::parse("pthd:unnamed");
  int revision{1};
  std::string name;
  std::string short_name;

  // world.placement: a region id, or the bonus route.
  bool bonus{false};
  std::string region;
  std::string bonus_route;
  std::optional<GeoPoint> geo;
  // Discovery tags: display.tags and every facet value (countries, states,
  // communities, biomes, settings, seasons). Never identity, never a
  // simulation input (location-contract.md section 10.5).
  std::vector<std::string> tags;

  // Tile codes, index y * 160 + x.
  std::vector<std::uint8_t> tiles;
  std::vector<DistrictDefinition> districts;
  // District index per 8 x 8 cell, index row * 20 + column.
  std::array<std::uint8_t, cell_count> district_cells{};
  std::vector<PropertyDefinition> properties;
  ActorDefinitions actors;
  std::vector<SupplierDefinition> food_suppliers;
  std::array<std::int64_t, ingredient_price_count> ingredient_base_prices{};
  // church, bank, cinema, school, disco, bar, department_store, hotel: the
  // order of the named-building tiles 0x88 .. 0xF8.
  std::array<std::vector<std::string>, named_building_types> building_names;

  std::int64_t reference_cash_cents{0};
  int min_affordable_premises{1};

  std::string tile_atlas;
  std::string medium_atlas;
  std::optional<std::string> thumbnail;
  std::vector<AttachmentDefinition> attachments;

  // integrity.definition_sha256 as written, if the document has one.
  std::optional<std::string> declared_sha256;
  // SHA-256 of the canonical form without the integrity member, computed on
  // read; equal to declared_sha256 for an admitted document (INT-1).
  std::string sha256;
  // The canonical form of the whole document as read (sorted keys, compact,
  // integrity included), for embedding in a save.
  std::string canonical_document;

  [[nodiscard]] std::uint8_t tile(int x, int y) const {
    return tiles[static_cast<std::size_t>((y * grid_width) + x)];
  }
  [[nodiscard]] int district_of_tile(int x, int y) const {
    return district_cells[static_cast<std::size_t>(
        ((y / tiles_per_cell) * cell_columns) + (x / tiles_per_cell))];
  }
};

} // namespace core::content
