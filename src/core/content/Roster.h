/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/common/Ids.h"
#include "core/content/ClassicCities.h"
#include "core/content/ContentKey.h"

namespace core::content {

// Where a roster entry's city comes from.
enum class RosterSource {
  // One of the ten original cities, built from the player's game files.
  Classic,
  // A LocationDefinition carried by the game (save version 4, not yet).
  Definition,
};

struct RosterEntry {
  ContentKey key;
  RosterSource source;
  // The original city number; set exactly for Classic entries.
  std::optional<int> classic_number;

  friend bool operator==(const RosterEntry&, const RosterEntry&) = default;
};

// The locations one game is played in, in CityId order: entry i is the city
// with CityId{i} (location-contract.md section 2.3). A game gets its roster
// when it starts and keeps it unchanged until it ends; installing, renaming or
// removing content never changes the roster of a game in progress. A Roster
// therefore has no way to change once built.
class Roster {
public:
  // Kept small until the per-game simulation budget is measured on the
  // target phones (location-contract.md section 19, decision 6).
  static constexpr std::size_t max_size = 32;

  // The original ten cities, in their original order.
  [[nodiscard]] static Roster classic() {
    std::vector<RosterEntry> entries;
    entries.reserve(classic_cities.size());
    for (const ClassicCity& city : classic_cities) {
      entries.push_back(RosterEntry{
          .key = classic_key(CityId{city.number}),
          .source = RosterSource::Classic,
          .classic_number = city.number,
      });
    }
    return Roster(std::move(entries));
  }

  // A roster from entries, or nullopt when the entries break a rule: 1 to
  // max_size entries, no key twice, Classic entries carry the classic key and
  // number of one original city, other entries carry no number and stay out
  // of the classic and QA namespaces.
  [[nodiscard]] static std::optional<Roster>
  from_entries(std::vector<RosterEntry> entries) {
    if (entries.empty() || entries.size() > max_size) {
      return std::nullopt;
    }
    std::unordered_set<ContentKey> seen;
    for (const RosterEntry& entry : entries) {
      if (!seen.insert(entry.key).second) {
        return std::nullopt;
      }
      if (entry.source == RosterSource::Classic) {
        const auto id = classic_city_id(entry.key);
        if (!id || entry.classic_number != id->value) {
          return std::nullopt;
        }
      }
      else if (entry.classic_number ||
               entry.key.in_namespace(classic_namespace) ||
               entry.key.in_namespace(qa_namespace)) {
        return std::nullopt;
      }
    }
    return Roster(std::move(entries));
  }

  [[nodiscard]] std::size_t size() const { return entries_.size(); }

  [[nodiscard]] bool contains(CityId id) const {
    return id.value >= 0 &&
           static_cast<std::size_t>(id.value) < entries_.size();
  }

  // id must be in the roster.
  [[nodiscard]] const RosterEntry& entry(CityId id) const {
    assert(contains(id));
    return entries_[static_cast<std::size_t>(id.value)];
  }

  [[nodiscard]] std::optional<CityId> find(const ContentKey& key) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].key == key) {
        return CityId{static_cast<int>(i)};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] const std::vector<RosterEntry>& entries() const {
    return entries_;
  }

  // Exactly the original ten in their original order: a Classic game.
  [[nodiscard]] bool is_classic() const {
    if (entries_.size() != classic_cities.size()) {
      return false;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const RosterEntry& e = entries_[i];
      if (e.source != RosterSource::Classic ||
          e.classic_number != classic_cities[i].number ||
          e.key.str() != classic_cities[i].key) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(const Roster&, const Roster&) = default;

private:
  explicit Roster(std::vector<RosterEntry> entries)
      : entries_(std::move(entries)) {}

  std::vector<RosterEntry> entries_;
};

} // namespace core::content
