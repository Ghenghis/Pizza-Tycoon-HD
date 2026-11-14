/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/common/Ids.h"
#include "core/content/ClassicCities.h"
#include "core/content/ContentKey.h"
#include "core/content/LocationDefinition.h"

namespace core::content {

// Where a roster entry's city comes from.
enum class RosterSource {
  // One of the ten original cities, built from the player's game files.
  Classic,
  // A validated LocationDefinition the game carries with it (embedded in
  // its saves, location-contract.md section 6.3).
  Definition,
};

struct RosterEntry {
  ContentKey key;
  RosterSource source;
  // The original city number; set exactly for Classic entries.
  std::optional<int> classic_number;
  // What a Definition entry is built from; null for Classic entries.
  std::shared_ptr<const LocationDefinition> definition;

  // Entries are equal when they name the same place built from the same
  // content: the same definition hash, wherever the definition lives.
  friend bool operator==(const RosterEntry& a, const RosterEntry& b) {
    if (a.key != b.key || a.source != b.source ||
        a.classic_number != b.classic_number ||
        (a.definition == nullptr) != (b.definition == nullptr)) {
      return false;
    }
    return a.definition == nullptr ||
           a.definition->sha256 == b.definition->sha256;
  }
};

// The locations one game is played in, in CityId order: entry i is the city
// with CityId{i} (location-contract.md section 2.3). A game gets its roster
// when it starts and keeps it unchanged until it ends; installing, renaming or
// removing content never changes the roster of a game in progress. A Roster
// therefore has no way to change once built.
class Roster {
public:
  // The provisional per-game budget of location-contract.md section 2.3,
  // kept until it is measured on the target phones (section 19, decision 6).
  // A game can start with at most NUMBER_OF_CITIES entries for now, since the
  // per-city arrays keep that size (GameState::finalize_initialization).
  static constexpr std::size_t max_size = 48;

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
  // number of one original city and no definition, other entries carry no
  // number, stay out of the classic and QA namespaces and carry the
  // definition of their own key.
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
        if (!id || entry.classic_number != id->value || entry.definition) {
          return std::nullopt;
        }
      }
      else if (entry.classic_number ||
               entry.key.in_namespace(classic_namespace) ||
               entry.key.in_namespace(qa_namespace) || !entry.definition ||
               entry.definition->key != entry.key) {
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

  // The original city number of position id, if it holds an original city.
  [[nodiscard]] std::optional<int> classic_number(CityId id) const {
    return contains(id)
               ? entries_[static_cast<std::size_t>(id.value)].classic_number
               : std::nullopt;
  }

  // The name the game shows for position id: the original name of an
  // original city, the display name of a definition.
  [[nodiscard]] std::string_view display_name(CityId id) const {
    if (!contains(id)) {
      return "Unknown";
    }
    const RosterEntry& e = entries_[static_cast<std::size_t>(id.value)];
    if (e.classic_number) {
      return core::city_name(CityId{*e.classic_number});
    }
    return e.definition->name;
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
