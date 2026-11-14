/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common/Ids.h"
#include "core/content/ContentKey.h"
#include "core/content/LocationDefinition.h"
#include "core/content/Regions.h"
#include "core/finance/Money.h"

namespace core::content {

class Roster;
struct Scenario;

// The Expanded world map (location-contract.md section 10, contract step 9):
// the places of a game grouped by region, their markers on a regional
// overview and the paged list beside it. Everything here is pure: building,
// browsing and paging read nothing but the entries they are given, so they
// never advance time, travel, charge money, construct a City or draw a random
// number.

struct WorldEntry {
  ContentKey key = *ContentKey::parse("pthd:unnamed");
  std::string name;
  // The game's roster position, when the entry is a place of this game.
  std::optional<CityId> city;
  // The one primary region, from the placement; empty for a bonus route.
  std::optional<std::size_t> region;
  std::string bonus_route;
  std::optional<GeoPoint> geo;
  std::vector<std::string> tags;
};

// The places of a game's roster: definitions by their placement and
// coordinates, original cities by the Classic adapter (ClassicCities.h).
[[nodiscard]] std::vector<WorldEntry>
roster_world_entries(const Roster& roster);

// The sort key of a display name: Latin-1 letters without their marks,
// lower case (the contract's fold for the ISO-8859-1 names of 1.0).
[[nodiscard]] std::string fold_name(std::string_view name);

// The list order: folded name, then key.
[[nodiscard]] bool list_before(const WorldEntry& a, const WorldEntry& b);

inline constexpr std::size_t desktop_page_size = 12;
inline constexpr std::size_t phone_page_size = 6;
inline constexpr std::size_t marker_budget = 12;

// ceil(n / size); 0 when n is 0.
[[nodiscard]] constexpr int page_count(std::size_t n, std::size_t size) {
  return size == 0 ? 0 : static_cast<int>((n + size - 1) / size);
}

// Where a point falls on a region's overview, in basis points of its width
// and height (section 10.3); nullopt outside the region.
struct MapPoint {
  int x{0};
  int y{0};
};
[[nodiscard]] std::optional<MapPoint> marker_position(const GeoPoint& geo,
                                                      const Region& region);

// One marker on an overview: a single place, or a cluster of nearby ones
// (members in list order).
struct Marker {
  MapPoint at;
  std::vector<const WorldEntry*> members;
};

// The markers of region: every entry whose coordinates lie in its bounds,
// its own and its neighbours' (each still listed once, under its own
// region). Up to budget markers; with more places they merge into the
// non-empty cells of a 4 x 3 grid.
[[nodiscard]] std::vector<Marker>
region_markers(const std::vector<WorldEntry>& entries, std::size_t region,
               std::size_t budget = marker_budget);

// Travel fares between places of a game (contract step 9).
struct TravelRates {
  // Thousandths of a dollar per degree. 4,000 matches the original map: Paris
  // to Berlin is 59 map units there and 14.7 degrees here, about 4 dollars a
  // degree.
  std::int64_t dollars_per_degree_milli = 4000;
  // The fare to or from a place with no coordinates (a bonus route).
  std::int64_t bonus_route_dollars = 600;
};
[[nodiscard]] TravelRates travel_rates(const Scenario* scenario);

// The fare from one place to another, deterministic integer arithmetic:
//   degrees_e6 = min(|dlon|, 360e6 - |dlon|) + |dlat|      (micro-degrees)
//   base       = degrees_e6 * dollars_per_degree_milli div 1e9   (dollars)
//   base       = bonus_route_dollars if either place has no coordinates
//   fare       = base * (difficulty + 1)                  (as EuroMap.cpp)
// Zero to the same place.
[[nodiscard]] Money travel_cost(const WorldEntry& from, const WorldEntry& to,
                                const TravelRates& rates, int difficulty);

// The browsing state of the world map: one view (a region, or the bonus
// routes), its list in list order, the page and the selection.
class WorldBrowser {
public:
  static constexpr std::size_t bonus_view = regions.size();
  static constexpr std::size_t view_count = regions.size() + 1;

  WorldBrowser(std::vector<WorldEntry> entries, std::size_t page_size);

  [[nodiscard]] const std::vector<WorldEntry>& entries() const {
    return entries_;
  }
  [[nodiscard]] std::size_t count_in(std::size_t view) const;

  // Shows view; keeps the selection if it is in the new list, else clears
  // it, and goes to the page of the selection or the first page.
  void show(std::size_t view);
  [[nodiscard]] std::size_t view() const { return view_; }

  [[nodiscard]] const std::vector<const WorldEntry*>& list() const {
    return list_;
  }
  [[nodiscard]] std::size_t page_size() const { return page_size_; }
  [[nodiscard]] int pages() const {
    return page_count(list_.size(), page_size_);
  }
  // 1-based; 0 when the list is empty.
  [[nodiscard]] int page() const { return page_; }
  void page(int page);
  [[nodiscard]] bool paging_enabled() const { return pages() > 1; }
  [[nodiscard]] std::vector<const WorldEntry*> page_entries() const;

  [[nodiscard]] const WorldEntry* selected() const;
  bool select(const ContentKey& key);
  // Through the list order across pages; no wrap. False at the ends.
  bool next();
  bool previous();
  [[nodiscard]] bool can_next() const;
  [[nodiscard]] bool can_previous() const;

  [[nodiscard]] std::vector<Marker>
  markers(std::size_t budget = marker_budget) const;

private:
  void rebuild();
  [[nodiscard]] std::optional<std::size_t> selected_index() const;
  void follow_selection();

  std::vector<WorldEntry> entries_;
  std::size_t page_size_;
  std::size_t view_{0};
  std::vector<const WorldEntry*> list_;
  int page_{0};
  std::optional<ContentKey> selected_;
};

} // namespace core::content
