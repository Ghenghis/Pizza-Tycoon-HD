/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/LocationValidator.h"

#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "core/content/LocationReader.h"
#include "core/content/Regions.h"
#include "core/food/IngredientType.h"

namespace core::content {

namespace tiles {

std::uint8_t out_directions(std::uint8_t t) {
  switch (t) {
  case 0x06:
  case 0xD6:
  case 0x17:
  case 0xA7:
  case 0xB7:
  case 0x47:
  case 0x86:
    return West;
  case 0x16:
  case 0xC6:
  case 0x97:
  case 0xD7:
  case 0xA6:
    return East;
  case 0x36:
  case 0x37:
  case 0x07:
  case 0xC7:
  case 0x96:
  case 0x57:
    return North;
  case 0x26:
  case 0xF7:
  case 0xE7:
  case 0xB6:
  case 0x67:
    return South;
  case 0x46:
  case 0x87:
    return West | South;
  case 0x56:
    return South | East;
  case 0x66:
  case 0x27:
    return West | North;
  case 0x76:
    return East | North;
  default:
    return 0;
  }
}

bool is_corner(std::uint8_t t) {
  switch (t) {
  case 0x46:
  case 0x56:
  case 0x66:
  case 0x76:
  case 0x86:
  case 0x96:
  case 0xA6:
  case 0xB6:
  case 0x27:
  case 0x57:
  case 0x67:
  case 0x87:
    return true;
  default:
    return false;
  }
}

} // namespace tiles

namespace {

using tiles::Direction;

// Cars cross this road code in a straight line: the original maps use it 529
// times as the middle of junctions, so it is part of the road vocabulary
// rather than an error (calibrated against the ten Classic maps).
constexpr std::uint8_t pass_through_road = 0x77;

// More than this many findings of one rule are summed up in one more.
constexpr int max_findings_per_rule = 40;

struct Step {
  Direction direction;
  int dx;
  int dy;
};

constexpr std::array<Step, 4> steps = {{
    {Direction::North, 0, -1},
    {Direction::East, 1, 0},
    {Direction::South, 0, 1},
    {Direction::West, -1, 0},
}};

constexpr std::uint8_t reverse(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::South;
  case Direction::East:
    return Direction::West;
  case Direction::South:
    return Direction::North;
  case Direction::West:
    return Direction::East;
  }
  return 0;
}

constexpr bool inside(int x, int y) {
  return x >= 0 && x < grid_width && y >= 0 && y < grid_height;
}

std::string xy(int x, int y) {
  return std::to_string(x) + "," + std::to_string(y);
}

std::string tile_pointer(int y) { return "/tiles/" + std::to_string(y); }

std::string hex_code(std::uint8_t t) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string out = "0x";
  out += digits[t >> 4];
  out += digits[t & 0x0F];
  return out;
}

class Report {
public:
  explicit Report(Findings& out) : out_(out) {}

  void add(std::string_view rule, Severity severity, std::string pointer,
           std::string message) {
    int& count = counts_[std::string(rule)];
    ++count;
    if (count <= max_findings_per_rule) {
      out_.push_back(Finding{.rule = std::string(rule),
                             .severity = severity,
                             .pointer = std::move(pointer),
                             .message = std::move(message)});
    }
    else {
      severities_[std::string(rule)] = severity;
    }
  }
  void error(std::string_view rule, std::string pointer, std::string message) {
    add(rule, Severity::Error, std::move(pointer), std::move(message));
  }
  void warning(std::string_view rule, std::string pointer,
               std::string message) {
    add(rule, Severity::Warning, std::move(pointer), std::move(message));
  }

  void close() {
    for (const auto& [rule, count] : counts_) {
      if (count > max_findings_per_rule) {
        out_.push_back(
            Finding{.rule = rule,
                    .severity = severities_[rule],
                    .pointer = "",
                    .message = std::to_string(count - max_findings_per_rule) +
                               " more like this"});
      }
    }
  }

private:
  Findings& out_;
  std::map<std::string, int> counts_;
  std::map<std::string, Severity> severities_;
};

constexpr std::array<std::string_view, named_building_types>
    building_type_names = {"church", "bank", "cinema",           "school",
                           "disco",  "bar",  "department_store", "hotel"};

class Validator {
public:
  Validator(const LocationDefinition& def, Findings& out)
      : def_(def), report_(out) {}

  void run() {
    identity();
    references();
    tile_codes();
    roads();
    districts();
    properties();
    actors();
    suppliers();
    names();
    economy();
    world();
    assets();
    integrity();
    report_.close();
  }

private:
  [[nodiscard]] std::uint8_t at(int x, int y) const { return def_.tile(x, y); }

  void identity() {
    std::set<std::string> seen;
    const auto check = [&](const std::string& id, const std::string& where) {
      if (!seen.insert(id).second) {
        report_.error("ID-1", where, "the id '" + id + "' is used twice");
      }
    };
    for (std::size_t i = 0; i < def_.districts.size(); ++i) {
      check(def_.districts[i].id, "/districts/" + std::to_string(i));
    }
    for (std::size_t i = 0; i < def_.properties.size(); ++i) {
      check(def_.properties[i].id, "/properties/" + std::to_string(i));
    }
    for_each_actor([&](const ActorDefinition& a, const std::string& where) {
      check(a.id, where);
    });
    for (std::size_t i = 0; i < def_.food_suppliers.size(); ++i) {
      check(def_.food_suppliers[i].id, "/food_suppliers/" + std::to_string(i));
    }

    std::set<int> numbers;
    for (std::size_t i = 0; i < def_.food_suppliers.size(); ++i) {
      if (!numbers.insert(def_.food_suppliers[i].number).second) {
        report_.error("ID-2", "/food_suppliers/" + std::to_string(i),
                      "supplier number " +
                          std::to_string(def_.food_suppliers[i].number) +
                          " is used twice");
      }
    }
    std::set<std::uint32_t> phones;
    for_each_actor([&](const ActorDefinition& a, const std::string& where) {
      if (a.phone != 0 && !phones.insert(a.phone).second) {
        report_.error("ID-2", where,
                      "phone number " + std::to_string(a.phone) +
                          " is used twice");
      }
    });

    if (def_.key.in_namespace("qa-synthetic")) {
      report_.error("ID-3", "/location_key",
                    "qa-synthetic content is for UI tests and is never built");
    }
  }

  template <typename F> void for_each_actor(F&& f) const {
    const auto list = [&](const std::vector<ActorDefinition>& actors,
                          std::string_view name) {
      for (std::size_t i = 0; i < actors.size(); ++i) {
        f(actors[i], "/actors/" + std::string(name) + "/" + std::to_string(i));
      }
    };
    const auto& a = def_.actors;
    list(a.realtors, "realtors");
    list(a.banks, "banks");
    list(a.insurers, "insurers");
    if (a.mayor) {
      f(*a.mayor, std::string("/actors/mayor"));
    }
    list(a.police, "police");
    list(a.mafia, "mafia");
    list(a.arms_dealers, "arms_dealers");
  }

  void references() {
    std::set<std::string> realtors;
    for (const auto& r : def_.actors.realtors) {
      realtors.insert(r.id);
    }
    for (std::size_t i = 0; i < def_.properties.size(); ++i) {
      const auto& p = def_.properties[i];
      if (!realtors.contains(p.realtor)) {
        report_.error("REF-1", "/properties/" + std::to_string(i) + "/realtor",
                      "'" + p.realtor + "' is not a realtor");
      }
    }
    const auto count = def_.districts.size();
    for (int r = 0; r < cell_rows; ++r) {
      for (int c = 0; c < cell_columns; ++c) {
        const auto value = def_.district_cells[(r * cell_columns) + c];
        if (value >= count) {
          report_.error("REF-2", "/district_cells/" + std::to_string(r),
                        "cell " + xy(c, r) + " names district " +
                            std::to_string(value) + " of " +
                            std::to_string(count));
        }
      }
    }
  }

  void tile_codes() {
    for (int y = 0; y < grid_height; ++y) {
      for (int x = 0; x < grid_width; ++x) {
        const auto t = at(x, y);
        if (tiles::is_owned_front(t)) {
          report_.error("TILE-1", tile_pointer(y),
                        "player shop front " + hex_code(t) + " at " + xy(x, y));
        }
        if (t == 0xE6 || t == 0xF6) {
          report_.warning("TILE-2", tile_pointer(y),
                          "unknown road-like code " + hex_code(t) + " at " +
                              xy(x, y));
        }
        if (t == 0xB7 || t == 0x67 || t == 0x87) {
          report_.warning("TILE-3", tile_pointer(y),
                          hex_code(t) + " at " + xy(x, y) +
                              " spawns cars in another direction than it "
                              "drives them");
        }
      }
    }
  }

  void roads() {
    component_.assign(grid_tiles, -1);
    std::vector<int> sizes;
    bool any = false;
    for (int y = 0; y < grid_height; ++y) {
      for (int x = 0; x < grid_width; ++x) {
        const auto t = at(x, y);
        if (!tiles::is_road(t)) {
          continue;
        }
        any = true;
        continuity(x, y, t);
        pairing(x, y, t);
        if (component_[(y * grid_width) + x] < 0) {
          sizes.push_back(flood(x, y, static_cast<int>(sizes.size())));
        }
      }
    }
    if (!any) {
      report_.error("ROAD-1", "/tiles", "the map has no road");
      return;
    }
    main_network_ = 0;
    for (std::size_t i = 1; i < sizes.size(); ++i) {
      if (sizes[i] > sizes[static_cast<std::size_t>(main_network_)]) {
        main_network_ = static_cast<int>(i);
      }
    }
    if (sizes.size() > 1) {
      report_.error(
          "ROAD-3", "/tiles",
          std::to_string(sizes.size()) +
              " separate road networks; the largest has " +
              std::to_string(sizes[static_cast<std::size_t>(main_network_)]) +
              " tiles");
    }
  }

  void continuity(int x, int y, std::uint8_t t) {
    const auto outs = tiles::out_directions(t);
    for (const Step& s : steps) {
      if ((outs & s.direction) == 0) {
        continue;
      }
      int nx = x + s.dx;
      int ny = y + s.dy;
      while (inside(nx, ny) && at(nx, ny) == pass_through_road) {
        nx += s.dx;
        ny += s.dy;
      }
      if (!inside(nx, ny)) {
        continue; // leaves the map: allowed
      }
      const auto next = at(nx, ny);
      const auto next_outs =
          tiles::is_road(next) ? tiles::out_directions(next) : 0;
      if (next_outs == 0) {
        report_.error("ROAD-2", tile_pointer(y),
                      "dead end: the lane at " + xy(x, y) + " runs into " +
                          xy(nx, ny));
      }
      else if ((next_outs & reverse(s.direction)) != 0) {
        report_.error("ROAD-2", tile_pointer(y),
                      "U-turn: the lane at " + xy(x, y) + " meets " +
                          xy(nx, ny) + " driving back");
      }
    }
  }

  void pairing(int x, int y, std::uint8_t t) {
    const auto outs = tiles::out_directions(t);
    if (tiles::is_corner(t) || outs == 0 || (outs & (outs - 1)) != 0) {
      return;
    }
    // A car leaving the view turns into the lane beside it (Car.cpp:87-104).
    int rx = x;
    int ry = y;
    switch (outs) {
    case Direction::East:
      ry -= 1;
      break;
    case Direction::West:
      ry += 1;
      break;
    case Direction::North:
      rx -= 1;
      break;
    case Direction::South:
      rx += 1;
      break;
    default:
      return;
    }
    if (inside(rx, ry) && !tiles::is_road(at(rx, ry))) {
      report_.error("ROAD-4", tile_pointer(y),
                    "the lane at " + xy(x, y) + " has no return lane at " +
                        xy(rx, ry));
    }
  }

  int flood(int x, int y, int id) {
    std::vector<std::pair<int, int>> stack{{x, y}};
    component_[(y * grid_width) + x] = id;
    int size = 0;
    while (!stack.empty()) {
      const auto [cx, cy] = stack.back();
      stack.pop_back();
      ++size;
      for (const Step& s : steps) {
        const int nx = cx + s.dx;
        const int ny = cy + s.dy;
        if (inside(nx, ny) && tiles::is_road(at(nx, ny)) &&
            component_[(ny * grid_width) + nx] < 0) {
          component_[(ny * grid_width) + nx] = id;
          stack.emplace_back(nx, ny);
        }
      }
    }
    return size;
  }

  [[nodiscard]] bool on_main_network(int x, int y) const {
    return inside(x, y) && main_network_ >= 0 &&
           component_[(y * grid_width) + x] == main_network_;
  }

  void districts() {
    const auto count = def_.districts.size();
    if (count < 1 || count > max_districts) {
      report_.error("DIST-1", "/districts",
                    std::to_string(count) + " districts; 1 to 16 are needed");
    }
    std::vector<bool> used(count, false);
    for (const auto value : def_.district_cells) {
      if (value < count) {
        used[value] = true;
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto& d = def_.districts[i];
      const std::string where = "/districts/" + std::to_string(i);
      if (!used[i]) {
        report_.warning("DIST-2", where,
                        "district '" + d.id + "' covers no cell");
      }
      int total = 0;
      for (const int w : d.population_weights_milli) {
        total += w;
      }
      if (total == 0) {
        report_.error("DIST-3", where,
                      "district '" + d.id + "' has no population");
      }
      if (d.price_level > max_price_level) {
        report_.error("ECON-2", where,
                      "price level " + std::to_string(d.price_level) +
                          " overflows the premise price");
      }
    }
  }

  [[nodiscard]] const DistrictDefinition* district_at(int x, int y) const {
    const auto index = static_cast<std::size_t>(def_.district_of_tile(x, y));
    return index < def_.districts.size() ? &def_.districts[index] : nullptr;
  }

  void properties() {
    std::map<int, std::size_t> by_tile;
    for (std::size_t i = 0; i < def_.properties.size(); ++i) {
      const auto& p = def_.properties[i];
      if (!by_tile.emplace(p.tile_index(), i).second) {
        report_.error("PROP-1", "/properties/" + std::to_string(i),
                      "a second property on tile " + xy(p.x, p.y));
      }
    }
    for (int y = 0; y < grid_height; ++y) {
      for (int x = 0; x < grid_width; ++x) {
        const auto t = at(x, y);
        const bool premise = tiles::is_premise(t);
        if (!premise && t != tiles::depot) {
          continue;
        }
        const auto it = by_tile.find((y * grid_width) + x);
        if (it == by_tile.end()) {
          report_.error("PROP-1", tile_pointer(y),
                        "the premise tile at " + xy(x, y) + " has no property");
          continue;
        }
        const auto kind = def_.properties[it->second].kind;
        const auto expected =
            premise ? PropertyType::BUSINESS_PREMISE : PropertyType::DEPOT;
        if (kind != expected) {
          report_.error("PROP-1", "/properties/" + std::to_string(it->second),
                        "the property's kind does not match tile " +
                            hex_code(t) + " at " + xy(x, y));
        }
      }
    }

    int business = 0;
    for (std::size_t i = 0; i < def_.properties.size(); ++i) {
      const auto& p = def_.properties[i];
      const std::string where = "/properties/" + std::to_string(i);
      const auto t = at(p.x, p.y);
      if (!tiles::is_premise(t) && t != tiles::depot) {
        report_.error("PROP-1", where,
                      "property '" + p.id + "' stands on tile " + hex_code(t) +
                          ", which is no premise");
      }
      if (p.entrance_offset > p.interior_w - 2) {
        report_.error("PROP-2", where,
                      "the doorway of '" + p.id +
                          "' reaches past the bottom wall");
      }
      if (p.kitchen_entry_offset > p.kitchen_h - 7) {
        report_.error("PROP-2", where,
                      "the kitchen door of '" + p.id +
                          "' is outside the kitchen wall");
      }
      if (p.kitchen_h < p.interior_w - 3 || p.kitchen_h > p.interior_w) {
        report_.warning("PROP-2", where,
                        "the kitchen height of '" + p.id +
                            "' is outside the Classic range");
      }
      if (!street_access(p)) {
        report_.error("PROP-3", where,
                      "'" + p.id + "' has no street on its " +
                          side_name(p.street_side) + " side");
      }
      if (const auto* d = district_at(p.x, p.y)) {
        const std::int64_t expected = std::int64_t{d->price_level} *
                                      p.interior_w * p.interior_h * 100 *
                                      (p.kind == PropertyType::DEPOT ? 5 : 1);
        if (expected != p.purchase_price_cents) {
          report_.warning("PROP-4", where,
                          "the price of '" + p.id +
                              "' differs from the Classic formula (" +
                              std::to_string(expected) + ")");
        }
      }
      if (p.kind == PropertyType::BUSINESS_PREMISE) {
        ++business;
      }
    }
    if (business < min_business_premises) {
      report_.error("PROP-5", "/properties",
                    std::to_string(business) +
                        " business premises; at least 16 are needed");
    }
  }

  static std::string side_name(StreetSide side) {
    switch (side) {
    case StreetSide::North:
      return "north";
    case StreetSide::East:
      return "east";
    case StreetSide::South:
      return "south";
    case StreetSide::West:
      return "west";
    }
    return "?";
  }

  [[nodiscard]] bool street_access(const PropertyDefinition& p) const {
    const Step& s = steps[static_cast<std::size_t>(p.street_side)];
    const int sx = p.x + s.dx;
    const int sy = p.y + s.dy;
    if (!inside(sx, sy)) {
      return false;
    }
    const auto t = at(sx, sy);
    if (tiles::is_road(t)) {
      return on_main_network(sx, sy);
    }
    if (tiles::is_path(t)) {
      for (const Step& n : steps) {
        if (on_main_network(sx + n.dx, sy + n.dy)) {
          return true;
        }
      }
    }
    return false;
  }

  void actors() {
    const auto& a = def_.actors;
    const auto count = [&](std::string_view name, std::size_t size,
                           std::size_t lo, std::size_t hi) {
      if (size < lo || size > hi) {
        report_.error("ACT-1", "/actors/" + std::string(name),
                      std::to_string(size) + " " + std::string(name) + "; " +
                          std::to_string(lo) + " to " + std::to_string(hi) +
                          " are needed");
      }
    };
    count("realtors", a.realtors.size(), 1, 10);
    count("banks", a.banks.size(), 1, 10);
    count("insurers", a.insurers.size(), 1, 10);
    count("police", a.police.size(), 1, 9);
    count("mafia", a.mafia.size(), 0, 2);
    count("arms_dealers", a.arms_dealers.size(), 0, 8);
    if (!a.mayor) {
      report_.error("ACT-1", "/actors/mayor", "the city has no mayor");
    }
    if (a.mafia.size() + a.arms_dealers.size() > 10) {
      report_.error("ACT-1", "/actors",
                    "more than 10 mafia and arms dealers together");
    }
  }

  void suppliers() {
    using core::food::ingredient_range_for_category;
    using UT = std::underlying_type_t<core::food::IngredientType>;
    for (int c = 0; c < static_cast<int>(ingredient_category_count); ++c) {
      const auto category = static_cast<IngredientCategory>(c);
      bool any = false;
      for (const auto& s : def_.food_suppliers) {
        any = any || s.category == category;
      }
      if (!any) {
        report_.error("SUP-1", "/food_suppliers",
                      "nobody supplies category " + std::to_string(c));
      }
      const auto range = ingredient_range_for_category(category);
      const auto first = static_cast<int>(static_cast<UT>(range.first));
      const auto last = static_cast<int>(static_cast<UT>(range.last));
      for (int id = first; id <= last; ++id) {
        const auto k = static_cast<std::size_t>(id - first);
        if (def_.ingredient_base_prices[static_cast<std::size_t>(id)] <= 0) {
          report_.error(
              "SUP-3", "/ingredient_base_prices/" + std::to_string(id),
              "ingredient " + std::to_string(id) + " has no base price");
        }
        bool sold = false;
        for (const auto& s : def_.food_suppliers) {
          sold = sold || (s.category == category && k < s.price_levels.size() &&
                          s.price_levels[k] > 0);
        }
        if (!sold) {
          report_.warning("SUP-2", "/food_suppliers",
                          "nobody sells ingredient " + std::to_string(id));
        }
      }
    }
  }

  void names() {
    std::array<bool, named_building_types> present{};
    for (const auto t : def_.tiles) {
      if (tiles::is_named_building(t)) {
        present[static_cast<std::size_t>((t >> 4) - 8)] = true;
      }
    }
    for (std::size_t i = 0; i < named_building_types; ++i) {
      if (present[i] && def_.building_names[i].empty()) {
        report_.warning(
            "NAME-1", "/building_names/" + std::string(building_type_names[i]),
            "the map has a " + std::string(building_type_names[i]) +
                " but no name for it");
      }
    }
  }

  void economy() {
    int affordable = 0;
    for (const auto& p : def_.properties) {
      if (p.kind != PropertyType::BUSINESS_PREMISE) {
        continue;
      }
      const auto* d = district_at(p.x, p.y);
      if (!d) {
        continue;
      }
      const std::int64_t rent_cents =
          std::int64_t{d->rent_level} * p.interior_w * p.interior_h * 100;
      const bool rentable = p.initial_status == PremiseStatus::FOR_RENT ||
                            p.initial_status == PremiseStatus::FOR_SALE_RENT;
      const bool for_sale = p.initial_status == PremiseStatus::FOR_SALE ||
                            p.initial_status == PremiseStatus::FOR_SALE_RENT;
      if (rentable && rent_cents * 3 <= def_.reference_cash_cents) {
        ++affordable;
      }
      else if (for_sale &&
               p.purchase_price_cents <= def_.reference_cash_cents) {
        ++affordable;
      }
    }
    if (affordable < def_.min_affordable_premises) {
      report_.error("ECON-1", "/economy/startup",
                    std::to_string(affordable) +
                        " premises are affordable with the reference cash; " +
                        std::to_string(def_.min_affordable_premises) +
                        " are needed");
    }
  }

  void world() {
    if (def_.bonus) {
      if (def_.geo) {
        report_.error("WORLD-2", "/world/geo",
                      "a bonus destination has no place on the globe");
      }
      return;
    }
    if (!def_.geo) {
      return;
    }
    for (const auto& r : regions) {
      if (r.id != def_.region) {
        continue;
      }
      const auto& g = *def_.geo;
      if (!region_contains(r, g.lon_e6, g.lat_e6)) {
        report_.warning("WORLD-1", "/world/geo",
                        "outside the " + std::string(r.id) +
                            " overview; reachable from a neighbour only");
      }
    }
  }

  void assets() {
    std::map<std::string, const AttachmentDefinition*> by_id;
    for (std::size_t i = 0; i < def_.attachments.size(); ++i) {
      const auto& a = def_.attachments[i];
      by_id.emplace(a.id, &a);
      const auto normal =
          std::filesystem::path(a.path).lexically_normal().generic_string();
      if (normal.empty() || normal.starts_with("..") ||
          normal.starts_with("/") || normal.find(':') != std::string::npos) {
        report_.error("ASSET-1",
                      "/assets/attachments/" + std::to_string(i) + "/file/path",
                      "the path leaves the bundle");
      }
    }
    if (def_.thumbnail) {
      const auto it = by_id.find(*def_.thumbnail);
      if (it == by_id.end()) {
        report_.error("ASSET-2", "/assets/thumbnail",
                      "no attachment '" + *def_.thumbnail + "'");
      }
      else if (def_.declared_sha256 &&
               it->second->state != AttachmentState::Approved) {
        report_.error("ASSET-2", "/assets/thumbnail",
                      "an admitted location may only use approved art");
      }
    }
  }

  void integrity() {
    if (def_.declared_sha256 && *def_.declared_sha256 != def_.sha256) {
      report_.error("INT-1", "/integrity/definition_sha256",
                    "the document hashes to " + def_.sha256);
    }
  }

  // A price level above this overflows the int price at CityFactory.cpp:730
  // for a 20 x 20 depot (ECON-2).
  static constexpr int max_price_level = 10737;
  static constexpr int min_business_premises = 16;

  const LocationDefinition& def_;
  Report report_;
  std::vector<int> component_;
  int main_network_{-1};
};

} // namespace

Findings validate_location(const LocationDefinition& definition) {
  Findings findings;
  Validator(definition, findings).run();
  return findings;
}

namespace {
LocationReport finish(LocationReadResult read) {
  LocationReport report;
  report.findings = std::move(read.findings);
  if (!read.definition) {
    return report;
  }
  auto semantic = validate_location(*read.definition);
  report.findings.insert(report.findings.end(), semantic.begin(),
                         semantic.end());
  if (!has_errors(report.findings)) {
    report.definition = std::move(read.definition);
  }
  return report;
}
} // namespace

LocationReport load_location(const std::filesystem::path& path) {
  return finish(read_location_file(path));
}

LocationReport check_location_text(std::string_view text) {
  return finish(read_location_text(text));
}

} // namespace core::content
