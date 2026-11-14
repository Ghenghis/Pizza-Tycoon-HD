/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

// Enum values read from a save. A number cast straight to an enum can later
// index a table past its end, so every enum a save holds is read with one of
// these. A value the game never writes throws std::out_of_range, naming the
// field and the value, and the load that asked for it turns the save away.
//
// The readers also assert, next to each check, the enumerators they expect an
// enum to have (enum_runs below). If an enum gains or loses a value the build
// stops there, so whoever changed it can make sure the saves still read every
// value the game writes.

namespace save {

using json = nlohmann::json;

namespace detail {

[[noreturn]] inline void bad_enum(const json& value, std::string_view what) {
  throw std::out_of_range("the save has " + std::string(what) + " " +
                          value.dump() + ", which this build does not know");
}

// The whole number in value, which must fit E's underlying type.
template <typename E>
std::underlying_type_t<E> enum_number(const json& value,
                                      std::string_view what) {
  using U = std::underlying_type_t<E>;
  static_assert(sizeof(U) < sizeof(std::int64_t),
                "an enum read from a save fits in 32 bits");
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto u = value.get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(std::numeric_limits<U>::max())) {
      bad_enum(value, what);
    }
    number = static_cast<std::int64_t>(u);
  }
  else if (value.is_number_integer()) {
    number = value.get<std::int64_t>();
  }
  else {
    bad_enum(value, what);
  }
  if (number < static_cast<std::int64_t>(std::numeric_limits<U>::min()) ||
      number > static_cast<std::int64_t>(std::numeric_limits<U>::max())) {
    bad_enum(value, what);
  }
  return static_cast<U>(number);
}

} // namespace detail

// value as E, when E has an enumerator with that value. Only for enums whose
// values all lie in magic_enum's reflection range; the enum_runs assertion
// beside each use makes sure of that.
template <typename E> E checked_enum(const json& value, std::string_view what) {
  const auto parsed =
      magic_enum::enum_cast<E>(detail::enum_number<E>(value, what));
  if (!parsed) {
    detail::bad_enum(value, what);
  }
  return *parsed;
}

// value as E, when it lies from first to last, both included. For an enum a
// save holds unnamed values of, one that ends in a count that is no value of
// its own, or one past magic_enum's reflection range.
template <typename E>
E checked_enum(const json& value, E first, E last, std::string_view what) {
  const auto number = detail::enum_number<E>(value, what);
  if (number < magic_enum::enum_integer(first) ||
      number > magic_enum::enum_integer(last)) {
    detail::bad_enum(value, what);
  }
  return static_cast<E>(number);
}

// object's member key read with checked_enum, or fallback when it has none.
template <typename E>
E checked_enum_or(const json& object, std::string_view key, E fallback,
                  std::string_view what) {
  if (!object.contains(key)) {
    return fallback;
  }
  return checked_enum<E>(object.at(key), what);
}

// Whether magic_enum sees E's enumerators running from first to last with no
// value missing in between.
template <typename E> constexpr bool enum_runs(E first, E last) {
  constexpr auto values = magic_enum::enum_values<E>();
  return !values.empty() && values.front() == first && values.back() == last &&
         values.size() ==
             static_cast<std::size_t>(magic_enum::enum_integer(last) -
                                      magic_enum::enum_integer(first)) +
                 1;
}

} // namespace save
