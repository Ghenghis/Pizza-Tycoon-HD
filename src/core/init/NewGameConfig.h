/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/GameTypes.h"
#include "core/common/Ids.h"
#include "core/content/Roster.h"
#include "core/mission/MissionTarget.h"

#include <optional>
#include <string>
#include <vector>

namespace init {
struct PlayerSlot {
  std::string name;
  bool is_selected{false};
  bool is_human{true};
  int character_id{};
  std::string character_name;
};

enum class RestaurantSize { NONE, SMALL, BIG };
enum class DifficultyLevel {
  PUSHOVER = 1,
  EASY = 2,
  NORMAL = 3,
  DIFFICULT = 4,
  IMPOSSIBLE = 5,
};

struct Config {
  GameMode mode = GameMode::FREEMODE;
  RestaurantSize size = RestaurantSize::NONE;
  DifficultyLevel difficulty = DifficultyLevel::PUSHOVER;
  std::string chosen_character;
  core::CityId city_id{0};
  // The places the game is played in, fixed for the whole game when it
  // starts; city_id indexes it. The Classic roster is the only one a game can
  // start with until the city arrays follow the roster.
  core::content::Roster roster = core::content::Roster::classic();
  std::vector<PlayerSlot> players;
  // Target CV; engaged only for GameMode::MISSION_MODE, nullopt otherwise.
  std::optional<core::mission::MissionTarget> mission;
};

} // namespace init
