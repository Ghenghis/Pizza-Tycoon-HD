/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/common/Ids.h"
#include "core/content/LocationDefinition.h"

class City;

namespace core::content {

class Roster;

// Builds a City from a validated LocationDefinition (location-contract.md
// section 4). The static parts (map, cells, districts, properties exactly as
// realized, actors in Classic id order, suppliers, prices, building names)
// draw nothing of their own; the draws a Classic city makes besides
// populate_properties happen at the same points and in the same order: the
// mayor's account, the suppliers' opening stock (City::suppliers), then
// CityFactory::initialise_new_city and the job market. The definition must have
// passed validate_location without errors.
[[nodiscard]] std::unique_ptr<City>
build_city(core::CityId id, const LocationDefinition& definition,
           bool fix_legacy_bugs,
           const std::vector<std::pair<std::uint8_t, int>>& player_chars);

// Only the static parts, without any start-up and with empty supplier
// stock: for tests and previews that must not touch the simulation stream.
// The result is not playable.
[[nodiscard]] std::unique_ptr<City>
build_static_city(core::CityId id, const LocationDefinition& definition);

// The city at roster position id: from the player's files for a Classic
// entry, from its definition otherwise.
[[nodiscard]] std::unique_ptr<City> build_roster_city(
    const Roster& roster, core::CityId id, bool fix_legacy_bugs,
    const std::vector<std::pair<std::uint8_t, int>>& player_chars);

} // namespace core::content
