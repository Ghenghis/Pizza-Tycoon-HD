/*
 * SPDX-FileCopyrightText: 2026 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "core/time/GameDate.h"

namespace save {

using json = nlohmann::json;

// The last year a save may hold. Every date the game makes is a few decades
// past calendar_epoch_year; this only keeps the day count from overflowing.
inline constexpr int last_save_year = 9999;

// A date read from a save. GameDate indexes its month table with the month, so
// a damaged month must never reach it: anything that is not a calendar date
// throws, and the load that asked for it is rejected.
inline GameDate save_date(int year, int month, int day) {
  if (year < calendar_epoch_year || year > last_save_year || month < 1 ||
      month > 12 || day < 1 || day > days_in_month[month - 1]) {
    throw std::out_of_range("save has an invalid date");
  }
  return GameDate{year, month, day};
}

inline json date_to_json(const GameDate& d) {
  return {{"year", d.year()}, {"month", d.month()}, {"day", d.day()}};
}

inline GameDate json_to_date(const json& j) {
  return save_date(j.at("year").get<int>(), j.at("month").get<int>(),
                   j.at("day").get<int>());
}

} // namespace save
