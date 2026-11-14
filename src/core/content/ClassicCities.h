/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cassert>
#include <optional>
#include <string_view>

#include "core/city/CityNames.h"
#include "core/common/Ids.h"
#include "core/content/ContentKey.h"
#include "core/util/Constants.h"

namespace core::content {

// The ten original cities under their stable keys (location-contract.md
// section 2.2). number is the city id the original data files and today's
// saves use (dat/city{number}.pc, cities[].id); it is also the city's place
// in the Classic roster, so a Classic game keeps every id it had.
struct ClassicCity {
  int number;
  std::string_view key;
};

inline constexpr std::array<ClassicCity, NUMBER_OF_CITIES> classic_cities = {{
    {.number = 0, .key = "classic:paris"},
    {.number = 1, .key = "classic:berlin"},
    {.number = 2, .key = "classic:baltimore"},
    {.number = 3, .key = "classic:vienna"},
    {.number = 4, .key = "classic:chicago"},
    {.number = 5, .key = "classic:new-york"},
    {.number = 6, .key = "classic:madrid"},
    {.number = 7, .key = "classic:rome"},
    {.number = 8, .key = "classic:stockholm"},
    {.number = 9, .key = "classic:london"},
}};

namespace detail {

// The slug is the English name in lower case with spaces as '-'.
constexpr bool slug_matches_name(std::string_view slug, std::string_view name) {
  if (slug.size() != name.size()) {
    return false;
  }
  for (std::size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    else if (c == ' ') {
      c = '-';
    }
    if (c != slug[i]) {
      return false;
    }
  }
  return true;
}

constexpr bool classic_table_is_consistent() {
  for (std::size_t i = 0; i < classic_cities.size(); ++i) {
    const ClassicCity& city = classic_cities[i];
    if (city.number != static_cast<int>(i) || !ContentKey::is_valid(city.key)) {
      return false;
    }
    const std::string_view key = city.key;
    const std::size_t colon = key.find(':');
    if (key.substr(0, colon) != classic_namespace ||
        !slug_matches_name(key.substr(colon + 1), city_names[i])) {
      return false;
    }
  }
  return true;
}

} // namespace detail

static_assert(detail::classic_table_is_consistent(),
              "classic_cities must follow core::city_names, in id order");

// The key of an original city. id must be 0..NUMBER_OF_CITIES-1.
[[nodiscard]] inline ContentKey classic_key(CityId id) {
  assert(id.value >= 0 && id.value < NUMBER_OF_CITIES);
  // The table is checked at compile time, so this cannot fail.
  const auto index = static_cast<std::size_t>(id.value);
  return *ContentKey::parse(classic_cities[index].key);
}

// The original city a key names, or nullopt for any other key.
[[nodiscard]] inline std::optional<CityId>
classic_city_id(const ContentKey& key) {
  for (const ClassicCity& city : classic_cities) {
    if (key.str() == city.key) {
      return CityId{city.number};
    }
  }
  return std::nullopt;
}

} // namespace core::content
