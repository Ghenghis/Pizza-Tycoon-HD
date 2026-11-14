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
// draw no random numbers; the start-up after them is the Classic one
// (CityFactory::initialise_new_city, then the job market), in the Classic
// order, so the simulation stream is drawn exactly where a Classic city
// draws it. The definition must have passed validate_location without
// errors.
[[nodiscard]] std::unique_ptr<City>
build_city(core::CityId id, const LocationDefinition& definition,
           bool fix_legacy_bugs,
           const std::vector<std::pair<std::uint8_t, int>>& player_chars);

// Only the static parts, without any start-up: for tests and previews that
// must not touch the simulation stream. The result is not playable.
[[nodiscard]] std::unique_ptr<City>
build_static_city(core::CityId id, const LocationDefinition& definition);

// The city at roster position id: from the player's files for a Classic
// entry, from its definition otherwise.
[[nodiscard]] std::unique_ptr<City> build_roster_city(
    const Roster& roster, core::CityId id, bool fix_legacy_bugs,
    const std::vector<std::pair<std::uint8_t, int>>& player_chars);

} // namespace core::content
