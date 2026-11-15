/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/common/Ids.h"
#include "core/debug/command/CommandDef.h"

class GameContext;
class GameState;

namespace debug::commands {

// `world add-area`, `world areas`, `world area-rename`: the territories'
// area lists (core/content/AreaCatalog.h).
debug::command::CommandDef make_world_commands();

// Makes the territory known to the process's area catalog: its home
// location is the game's pinned one when the game holds it, otherwise the
// shipped one. Areas of that territory that the game holds and the catalog
// lacks are adopted. False, with why, for an unknown territory.
bool ensure_territory(GameState& state, std::string_view territory,
                      std::string* why);

// Where `nav area` goes: an area id ("area-000001") or a location key. An
// admitted area this game does not hold yet is added to the game's roster
// (an explicit expansion; added is then true). The city is built if needed.
// nullopt, with why, for an unknown or draft area (why lists its missing
// requirements) or a full roster.
std::optional<core::CityId> enter_area(GameState& state, GameContext& context,
                                       std::string_view id_or_key,
                                       std::string* why, bool* added);

} // namespace debug::commands
