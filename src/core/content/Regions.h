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
#include <string_view>

namespace core::content {

// The eight regional overviews, in their canonical order, with their crop
// bounds in micro-degrees (location-contract.md section 10.1,
// WORLD-REGIONS-AND-PAGINATION.md). A location belongs to exactly one region,
// by its placement; the bounds only decide where its marker is drawn and
// from which neighbouring overview it can also be found.
struct Region {
  std::string_view id;
  std::string_view label;
  std::int64_t west;
  std::int64_t south;
  std::int64_t east;
  std::int64_t north;
};

inline constexpr std::array<Region, 8> regions = {{
    {"north-america", "North America", -170000000, 10000000, -50000000,
     83000000},
    {"latin-america-caribbean", "Latin America and the Caribbean", -120000000,
     -60000000, -30000000, 33000000},
    {"europe", "Europe", -25000000, 32000000, 50000000, 74000000},
    {"north-africa-western-asia", "North Africa and Western Asia", -20000000,
     5000000, 65000000, 47000000},
    {"sub-saharan-africa", "Sub-Saharan Africa", -20000000, -36000000, 55000000,
     23000000},
    {"central-south-asia", "Central and South Asia", 42000000, -3000000,
     97000000, 58000000},
    {"east-southeast-asia", "East and Southeast Asia", 92000000, -14000000,
     155000000, 60000000},
    {"oceania-pacific", "Oceania and the Pacific", 108000000, -52000000,
     180000000, 8000000},
}};

[[nodiscard]] constexpr std::optional<std::size_t>
region_index(std::string_view id) {
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (regions[i].id == id) {
      return i;
    }
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool region_contains(const Region& r, std::int64_t lon,
                                             std::int64_t lat) {
  return lon >= r.west && lon <= r.east && lat >= r.south && lat <= r.north;
}

} // namespace core::content
