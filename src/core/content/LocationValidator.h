/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"

namespace core::content {

// Tile classes the rules use, by the engine's own predicates
// (location-contract.md section 3.1).
namespace tiles {
// City::is_road_tile (City.cpp:144-148).
[[nodiscard]] constexpr bool is_road(std::uint8_t t) {
  return ((t & 0x0F) == 0x06 || (t & 0x0F) == 0x07) && t != 0xE6 && t != 0xF6;
}
// City::is_named_building: church 0x88 .. hotel 0xF8.
[[nodiscard]] constexpr bool is_named_building(std::uint8_t t) {
  return (t & 0x0F) == 0x08 && (t >> 4) >= 0x8;
}
// City::is_business_premise_by_type: a low nibble of 0xA. High nibbles 0..7
// are player shop fronts, which definitions may not contain (TILE-1).
[[nodiscard]] constexpr bool is_premise(std::uint8_t t) {
  return (t & 0x0F) == 0x0A;
}
[[nodiscard]] constexpr bool is_owned_front(std::uint8_t t) {
  return is_premise(t) && (t >> 4) < 0x8;
}
inline constexpr std::uint8_t depot = 0x04;
// Footpaths (low nibble 5) except the depot building 0x05.
[[nodiscard]] constexpr bool is_path(std::uint8_t t) {
  return (t & 0x0F) == 0x05 && t != 0x05;
}

enum Direction : std::uint8_t {
  North = 1,
  East = 2,
  South = 4,
  West = 8,
};

// The directions a car may leave a road tile in (Car::decide_desired_direction,
// Car.cpp:171-223), as a mask of Direction. Zero for every other code,
// including 0x77, which cars cross in a straight line.
[[nodiscard]] std::uint8_t out_directions(std::uint8_t t);
// The one-way and two-way corner codes, which ROAD-4 leaves alone.
[[nodiscard]] bool is_corner(std::uint8_t t);
} // namespace tiles

// Stage 2 of the pipeline: every semantic rule of location-contract.md
// section 5 that a single document can be checked for, all findings
// reported. Catalog-time checks (the lineage half of ID-3) run in the catalog.
[[nodiscard]] Findings validate_location(const LocationDefinition& definition);

// Both stages: the definition when it has no error, plus every finding.
struct LocationReport {
  std::shared_ptr<const LocationDefinition> definition;
  Findings findings;
  [[nodiscard]] bool ok() const {
    return definition != nullptr && !has_errors(findings);
  }
};

[[nodiscard]] LocationReport load_location(const std::filesystem::path& path);
[[nodiscard]] LocationReport check_location_text(std::string_view text);

} // namespace core::content
