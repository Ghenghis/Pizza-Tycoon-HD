/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/WorldMap.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <utility>

#include "core/city/CityNames.h"
#include "core/content/ClassicCities.h"
#include "core/content/Roster.h"
#include "core/content/Scenario.h"

namespace core::content {

namespace {

constexpr std::int64_t full_turn_e6 = 360'000'000;
constexpr int basis = 10000;
constexpr int grid_columns = 4;
constexpr int grid_rows = 3;

// Folded form of a Latin-1 letter U+00C0..U+00FF.
std::string_view fold_latin1(char32_t cp) {
  static constexpr std::array<std::string_view, 64> table = {
      "a", "a", "a", "a", "a",  "a",  "ae", "c", "e", "e", "e",  "e", "i",
      "i", "i", "i", "d", "n",  "o",  "o",  "o", "o", "o", "x",  "o", "u",
      "u", "u", "u", "y", "th", "ss", "a",  "a", "a", "a", "a",  "a", "ae",
      "c", "e", "e", "e", "e",  "i",  "i",  "i", "i", "d", "n",  "o", "o",
      "o", "o", "o", "/", "o",  "u",  "u",  "u", "u", "y", "th", "y"};
  return table[static_cast<std::size_t>(cp - 0xC0)];
}

} // namespace

std::vector<WorldEntry> roster_world_entries(const Roster& roster) {
  std::vector<WorldEntry> out;
  for (std::size_t i = 0; i < roster.size(); ++i) {
    const RosterEntry& e = roster.entries()[i];
    WorldEntry w;
    w.key = e.key;
    w.city = CityId{static_cast<int>(i)};
    if (e.classic_number) {
      const auto& place =
          classic_places[static_cast<std::size_t>(*e.classic_number)];
      w.name = std::string(city_name(CityId{*e.classic_number}));
      w.region = region_index(place.region);
      w.geo = GeoPoint{.lon_e6 = place.lon_e6, .lat_e6 = place.lat_e6};
    }
    else {
      const auto& def = *e.definition;
      w.name = def.name;
      if (def.bonus) {
        w.bonus_route = def.bonus_route;
      }
      else {
        w.region = region_index(def.region);
      }
      w.geo = def.geo;
      w.tags = def.tags;
    }
    out.push_back(std::move(w));
  }
  return out;
}

std::string fold_name(std::string_view name) {
  std::string out;
  std::size_t i = 0;
  while (i < name.size()) {
    const auto c = static_cast<unsigned char>(name[i]);
    if (c < 0x80) {
      out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      ++i;
      continue;
    }
    // Two-byte UTF-8 covers U+0080..U+07FF, all Latin-1 included.
    if ((c & 0xE0) == 0xC0 && i + 1 < name.size()) {
      const char32_t cp = (static_cast<char32_t>(c & 0x1F) << 6) |
                          (static_cast<unsigned char>(name[i + 1]) & 0x3F);
      if (cp >= 0xC0 && cp <= 0xFF) {
        out += fold_latin1(cp);
      }
      else {
        out.append(name.substr(i, 2));
      }
      i += 2;
      continue;
    }
    out += static_cast<char>(c);
    ++i;
  }
  return out;
}

bool list_before(const WorldEntry& a, const WorldEntry& b) {
  const auto fa = fold_name(a.name);
  const auto fb = fold_name(b.name);
  if (fa != fb) {
    return fa < fb;
  }
  return a.key < b.key;
}

std::optional<MapPoint> marker_position(const GeoPoint& geo,
                                        const Region& region) {
  if (!region_contains(region, geo.lon_e6, geo.lat_e6)) {
    return std::nullopt;
  }
  const auto x =
      (geo.lon_e6 - region.west) * basis / (region.east - region.west);
  const auto y =
      (region.north - geo.lat_e6) * basis / (region.north - region.south);
  return MapPoint{.x = static_cast<int>(x), .y = static_cast<int>(y)};
}

std::vector<Marker> region_markers(const std::vector<WorldEntry>& entries,
                                   std::size_t region, std::size_t budget) {
  std::vector<const WorldEntry*> shown;
  for (const auto& e : entries) {
    if (e.geo &&
        region_contains(regions[region], e.geo->lon_e6, e.geo->lat_e6)) {
      shown.push_back(&e);
    }
  }
  std::sort(shown.begin(), shown.end(),
            [](const WorldEntry* a, const WorldEntry* b) {
              return list_before(*a, *b);
            });
  std::vector<Marker> out;
  if (shown.size() <= budget) {
    for (const auto* e : shown) {
      out.push_back(Marker{.at = *marker_position(*e->geo, regions[region]),
                           .members = {e}});
    }
    return out;
  }
  // Grid buckets in map space, filled in list order.
  std::map<int, Marker> cells;
  for (const auto* e : shown) {
    const auto p = *marker_position(*e->geo, regions[region]);
    const int col = std::min(grid_columns - 1, p.x * grid_columns / basis);
    const int row = std::min(grid_rows - 1, p.y * grid_rows / basis);
    cells[(row * grid_columns) + col].members.push_back(e);
  }
  for (auto& [cell, marker] : cells) {
    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (const auto* e : marker.members) {
      const auto p = *marker_position(*e->geo, regions[region]);
      sx += p.x;
      sy += p.y;
    }
    const auto n = static_cast<std::int64_t>(marker.members.size());
    marker.at =
        MapPoint{.x = static_cast<int>(sx / n), .y = static_cast<int>(sy / n)};
    out.push_back(std::move(marker));
  }
  return out;
}

TravelRates travel_rates(const Scenario* scenario) {
  TravelRates rates;
  if (scenario) {
    rates.dollars_per_degree_milli =
        scenario->travel_dollars_per_degree_milli.value_or(
            rates.dollars_per_degree_milli);
    rates.bonus_route_dollars =
        scenario->bonus_route_cost_dollars.value_or(rates.bonus_route_dollars);
  }
  return rates;
}

Money travel_cost(const WorldEntry& from, const WorldEntry& to,
                  const TravelRates& rates, int difficulty) {
  if (from.key == to.key) {
    return Money{0};
  }
  std::int64_t base = rates.bonus_route_dollars;
  if (from.geo && to.geo) {
    std::int64_t dlon = std::llabs(from.geo->lon_e6 - to.geo->lon_e6);
    dlon = std::min(dlon, full_turn_e6 - dlon);
    const std::int64_t dlat = std::llabs(from.geo->lat_e6 - to.geo->lat_e6);
    base = (dlon + dlat) * rates.dollars_per_degree_milli / 1'000'000'000;
  }
  return Money::from_dollars(base * (difficulty + 1));
}

WorldBrowser::WorldBrowser(std::vector<WorldEntry> entries,
                           std::size_t page_size)
    : entries_(std::move(entries)), page_size_(page_size) {
  std::sort(entries_.begin(), entries_.end(), list_before);
  rebuild();
}

std::size_t WorldBrowser::count_in(std::size_t view) const {
  return static_cast<std::size_t>(std::count_if(
      entries_.begin(), entries_.end(), [view](const WorldEntry& e) {
        return view == bonus_view ? !e.region.has_value() : e.region == view;
      }));
}

void WorldBrowser::rebuild() {
  list_.clear();
  for (const auto& e : entries_) {
    const bool in_view =
        view_ == bonus_view ? !e.region.has_value() : e.region == view_;
    if (in_view) {
      list_.push_back(&e);
    }
  }
  if (selected_ && !selected_index()) {
    selected_.reset();
  }
  follow_selection();
}

void WorldBrowser::follow_selection() {
  if (pages() == 0) {
    page_ = 0;
    return;
  }
  if (const auto index = selected_index()) {
    page_ = static_cast<int>(*index / page_size_) + 1;
    return;
  }
  page_ = std::min(std::max(page_, 1), pages());
}

void WorldBrowser::show(std::size_t view) {
  view_ = std::min(view, view_count - 1);
  page_ = 1;
  rebuild();
}

void WorldBrowser::page(int page) {
  page_ = pages() == 0 ? 0 : std::min(std::max(page, 1), pages());
}

std::vector<const WorldEntry*> WorldBrowser::page_entries() const {
  if (page_ == 0) {
    return {};
  }
  const auto first = static_cast<std::size_t>(page_ - 1) * page_size_;
  const auto last = std::min(first + page_size_, list_.size());
  return {list_.begin() + static_cast<std::ptrdiff_t>(first),
          list_.begin() + static_cast<std::ptrdiff_t>(last)};
}

std::optional<std::size_t> WorldBrowser::selected_index() const {
  if (!selected_) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < list_.size(); ++i) {
    if (list_[i]->key == *selected_) {
      return i;
    }
  }
  return std::nullopt;
}

const WorldEntry* WorldBrowser::selected() const {
  const auto index = selected_index();
  return index ? list_[*index] : nullptr;
}

bool WorldBrowser::select(const ContentKey& key) {
  for (const auto* e : list_) {
    if (e->key == key) {
      selected_ = key;
      follow_selection();
      return true;
    }
  }
  return false;
}

bool WorldBrowser::can_next() const {
  if (list_.empty()) {
    return false;
  }
  const auto index = selected_index();
  return !index || *index + 1 < list_.size();
}

bool WorldBrowser::can_previous() const {
  const auto index = selected_index();
  return index && *index > 0;
}

bool WorldBrowser::next() {
  if (!can_next()) {
    return false;
  }
  const auto index = selected_index();
  selected_ = list_[index ? *index + 1 : 0]->key;
  follow_selection();
  return true;
}

bool WorldBrowser::previous() {
  if (!can_previous()) {
    return false;
  }
  selected_ = list_[*selected_index() - 1]->key;
  follow_selection();
  return true;
}

std::vector<Marker> WorldBrowser::markers(std::size_t budget) const {
  if (view_ == bonus_view) {
    return {};
  }
  return region_markers(entries_, view_, budget);
}

} // namespace core::content
