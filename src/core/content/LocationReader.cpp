/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/LocationReader.h"

#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>

#include "core/content/JsonShape.h"
#include "core/content/Sha256.h"

namespace core::content {

namespace {

using json = nlohmann::json;

// A definition file is about 50 KB; anything far larger is not one.
constexpr std::uintmax_t max_file_bytes = 4u * 1024u * 1024u;

constexpr std::initializer_list<std::string_view> region_ids = {
    "north-america",
    "latin-america-caribbean",
    "europe",
    "north-africa-western-asia",
    "sub-saharan-africa",
    "central-south-asia",
    "east-southeast-asia",
    "oceania-pacific",
};

bool hex_value(char c, int& out) {
  if (c >= '0' && c <= '9') {
    out = c - '0';
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = c - 'a' + 10;
    return true;
  }
  return false;
}

bool lower_hex_row(std::string_view text) {
  if (text.size() != static_cast<std::size_t>(grid_width) * 2) {
    return false;
  }
  int ignored = 0;
  for (const char c : text) {
    if (!hex_value(c, ignored)) {
      return false;
    }
  }
  return true;
}

bool cell_row(std::string_view text) {
  if (text.size() != static_cast<std::size_t>(cell_columns) * 2) {
    return false;
  }
  int ignored = 0;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    if (text[i] != '0' || !hex_value(text[i + 1], ignored)) {
      return false;
    }
  }
  return true;
}

bool two_letters(std::string_view text) {
  return text.size() == 2 && text[0] >= 'a' && text[0] <= 'z' &&
         text[1] >= 'a' && text[1] <= 'z';
}

bool lower_or_digit(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// ^[a-z]{2}-[a-z0-9]{1,3}$
bool state_code(std::string_view text) {
  if (text.size() < 4 || text.size() > 6 || !two_letters(text.substr(0, 2)) ||
      text[2] != '-') {
    return false;
  }
  for (const char c : text.substr(3)) {
    if (!lower_or_digit(c)) {
      return false;
    }
  }
  return true;
}

// ^[a-z0-9][a-z0-9-]{1,47}$
bool community(std::string_view text) {
  if (text.size() < 2 || text.size() > 48 || !lower_or_digit(text[0])) {
    return false;
  }
  for (const char c : text.substr(1)) {
    if (!lower_or_digit(c) && c != '-') {
      return false;
    }
  }
  return true;
}

// ^kit:[a-z0-9][a-z0-9-]{1,47}$
bool kit_id(std::string_view text) {
  return text.size() > 4 && text.substr(0, 4) == "kit:" &&
         community(text.substr(4));
}

// ^(classic:gfx/[a-z0-9_]{1,16}|bundle:[A-Za-z0-9._/-]{1,200})$
bool asset_key(std::string_view text) {
  constexpr std::string_view classic_prefix = "classic:gfx/";
  constexpr std::string_view bundle_prefix = "bundle:";
  if (text.starts_with(classic_prefix)) {
    const auto rest = text.substr(classic_prefix.size());
    if (rest.empty() || rest.size() > 16) {
      return false;
    }
    for (const char c : rest) {
      if (!lower_or_digit(c) && c != '_') {
        return false;
      }
    }
    return true;
  }
  if (text.starts_with(bundle_prefix)) {
    const auto rest = text.substr(bundle_prefix.size());
    if (rest.empty() || rest.size() > 200) {
      return false;
    }
    for (const char c : rest) {
      const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '/' || c == '-';
      if (!ok) {
        return false;
      }
    }
    return true;
  }
  return false;
}

// ^[a-z][a-z0-9.-]{2,63}$ for template ids is patterns::dotted_id.

class LocationParser {
public:
  explicit LocationParser(Findings& findings) : shape_(findings) {}

  std::shared_ptr<const LocationDefinition> parse(const json& doc) {
    auto def = std::make_shared<LocationDefinition>();
    const std::string root;
    if (!shape_.object(
            doc, root,
            {"format",         "schema_version", "location_key",
             "revision",       "display",        "world",
             "facets",         "grid",           "tiles",
             "districts",      "district_cells", "properties",
             "actors",         "food_suppliers", "ingredient_base_prices",
             "building_names", "economy",        "assets",
             "provenance",     "integrity"},
            {"format", "schema_version", "location_key", "revision", "display",
             "world", "facets", "grid", "tiles", "districts", "district_cells",
             "properties", "actors", "food_suppliers", "ingredient_base_prices",
             "building_names", "economy", "assets", "provenance"}) &&
        !doc.is_object()) {
      return nullptr;
    }
    if (const json* v = JsonShape::member(doc, "format")) {
      shape_.constant(*v, "/format", "pthd.location");
    }
    if (const json* v = JsonShape::member(doc, "schema_version")) {
      shape_.constant(*v, "/schema_version", "1.0.0");
    }
    if (auto key = shape_.text_at(doc, root, "location_key", 1, 96, false,
                                  patterns::content_key)) {
      if (key->starts_with("classic:")) {
        shape_.fail("/location_key",
                    "the classic: namespace is reserved for the original ten");
      }
      else if (auto parsed = ContentKey::parse(*key)) {
        def->key = *parsed;
      }
    }
    if (auto revision =
            shape_.integer_at(doc, root, "revision", 1, 2147483647)) {
      def->revision = static_cast<int>(*revision);
    }
    if (const json* v = JsonShape::member(doc, "display")) {
      display(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "world")) {
      world(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "facets")) {
      facets(*v);
    }
    if (const json* v = JsonShape::member(doc, "grid")) {
      grid(*v);
    }
    if (const json* v = JsonShape::member(doc, "tiles")) {
      tiles(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "districts")) {
      districts(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "district_cells")) {
      district_cells(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "properties")) {
      properties(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "actors")) {
      actors(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "food_suppliers")) {
      suppliers(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "ingredient_base_prices")) {
      base_prices(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "building_names")) {
      building_names(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "economy")) {
      economy(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "assets")) {
      assets(*v, *def);
    }
    if (const json* v = JsonShape::member(doc, "provenance")) {
      provenance(*v);
    }
    if (const json* v = JsonShape::member(doc, "integrity")) {
      integrity(*v, *def);
    }
    if (shape_.failed()) {
      return nullptr;
    }
    collect_tags(doc, *def);
    def->sha256 = definition_sha256(doc);
    def->canonical_document = canonical_json(doc);
    return def;
  }

private:
  using Ptr = const std::string&;

  static std::string at(Ptr pointer, std::string_view key) {
    return JsonShape::child(pointer, key);
  }
  static std::string at(Ptr pointer, std::size_t index) {
    return JsonShape::child(pointer, index);
  }

  // An array of distinct values from options.
  void enum_set(const json& object, Ptr pointer, std::string_view key,
                std::size_t max_items,
                std::initializer_list<std::string_view> options) {
    const json* list = JsonShape::member(object, key);
    if (!list) {
      return;
    }
    const std::string p = at(pointer, key);
    if (!shape_.array(*list, p, 0, max_items)) {
      return;
    }
    std::set<std::size_t> seen;
    for (std::size_t i = 0; i < list->size(); ++i) {
      if (auto index = shape_.choice((*list)[i], at(p, i), options)) {
        if (!seen.insert(*index).second) {
          shape_.fail(at(p, i), "duplicate item");
        }
      }
    }
  }

  void string_set(const json& object, Ptr pointer, std::string_view key,
                  std::size_t max_items, std::size_t min_length,
                  std::size_t max_length, bool latin1,
                  JsonShape::Pattern pattern) {
    if (const json* list = JsonShape::member(object, key)) {
      shape_.string_set(*list, at(pointer, key), max_items, min_length,
                        max_length, latin1, pattern);
    }
  }

  // The shape is known good here.
  static void collect_tags(const json& doc, LocationDefinition& def) {
    const auto add_all = [&](const json& list) {
      for (const auto& item : list) {
        def.tags.push_back(item.get<std::string>());
      }
    };
    if (const auto& display = doc.at("display"); display.contains("tags")) {
      add_all(display.at("tags"));
    }
    for (const char* facet : {"countries", "states", "communities", "biomes",
                              "settings", "seasons"}) {
      add_all(doc.at("facets").at(facet));
    }
  }

  void display(const json& v, LocationDefinition& def) {
    const std::string p = "/display";
    if (!shape_.object(v, p, {"aliases", "tags"}, {"name", "short_name"})) {
      if (!v.is_object()) {
        return;
      }
    }
    def.name = shape_.text_at(v, p, "name", 1, 24, true).value_or("");
    def.short_name =
        shape_.text_at(v, p, "short_name", 1, 12, true).value_or("");
    string_set(v, p, "aliases", 8, 1, 24, true, nullptr);
    string_set(v, p, "tags", 16, 1, 32, false, patterns::tag);
  }

  void world(const json& v, LocationDefinition& def) {
    const std::string p = "/world";
    shape_.object(v, p, {"label_side"}, {"placement", "geo"});
    if (!v.is_object()) {
      return;
    }
    if (const json* placement = JsonShape::member(v, "placement")) {
      const std::string pp = at(p, "placement");
      const json* kind = JsonShape::member(*placement, "kind");
      if (!placement->is_object() || !kind) {
        shape_.fail(pp, "expected a region or bonus placement");
      }
      else if (*kind == "region") {
        shape_.object(*placement, pp, {}, {"kind", "region"});
        if (auto region =
                shape_.choice_at(*placement, pp, "region", region_ids)) {
          def.region = std::string(*(region_ids.begin() + *region));
        }
      }
      else if (*kind == "bonus") {
        shape_.object(*placement, pp, {}, {"kind", "route"});
        if (shape_.choice_at(*placement, pp, "route", {"north-pole"})) {
          def.bonus = true;
          def.bonus_route = "north-pole";
        }
      }
      else {
        shape_.fail(at(pp, "kind"), "not one of the allowed values");
      }
    }
    if (const json* geo = JsonShape::member(v, "geo"); geo && !geo->is_null()) {
      const std::string gp = at(p, "geo");
      if (shape_.object(*geo, gp, {"source_ref"},
                        {"lon_e6", "lat_e6", "source"})) {
        const auto lon =
            shape_.integer_at(*geo, gp, "lon_e6", -180000000, 180000000);
        const auto lat =
            shape_.integer_at(*geo, gp, "lat_e6", -90000000, 90000000);
        shape_.choice_at(*geo, gp, "source", {"gazetteer", "user-placed"});
        shape_.text_at(*geo, gp, "source_ref", 3, 64, false,
                       patterns::dotted_id);
        if (lon && lat) {
          def.geo = GeoPoint{.lon_e6 = *lon, .lat_e6 = *lat};
        }
      }
    }
    shape_.choice_at(v, p, "label_side", {"n", "e", "s", "w"});
  }

  void facets(const json& v) {
    const std::string p = "/facets";
    shape_.object(v, p, {},
                  {"countries", "states", "communities", "biomes", "settings",
                   "seasons"});
    string_set(v, p, "countries", 8, 2, 2, false, two_letters);
    string_set(v, p, "states", 8, 4, 6, false, state_code);
    string_set(v, p, "communities", 8, 2, 48, false, community);
    enum_set(v, p, "biomes", 4,
             {"temperate", "continental", "mediterranean", "arid", "semi-arid",
              "tropical", "subtropical", "highland", "polar",
              "fictional-winter"});
    enum_set(v, p, "settings", 4,
             {"dense", "urban", "metropolitan", "coastal", "inland", "hillside",
              "village"});
    enum_set(
        v, p, "seasons", 6,
        {"all-year", "winter", "spring", "summer", "autumn", "wet", "dry"});
  }

  void grid(const json& v) {
    const std::string p = "/grid";
    if (!shape_.object(v, p, {},
                       {"width", "height", "district_cell_columns",
                        "district_cell_rows", "tile_encoding"})) {
      if (!v.is_object()) {
        return;
      }
    }
    const auto check = [&](std::string_view key, std::int64_t expected) {
      if (const json* m = JsonShape::member(v, key)) {
        shape_.constant(*m, at(p, key), expected);
      }
    };
    check("width", grid_width);
    check("height", grid_height);
    check("district_cell_columns", cell_columns);
    check("district_cell_rows", cell_rows);
    if (const json* m = JsonShape::member(v, "tile_encoding")) {
      shape_.constant(*m, at(p, "tile_encoding"), "hex-rows-v1");
    }
  }

  void tiles(const json& v, LocationDefinition& def) {
    const std::string p = "/tiles";
    if (!shape_.array(v, p, grid_height, grid_height)) {
      return;
    }
    def.tiles.assign(grid_tiles, 0);
    for (std::size_t y = 0; y < v.size(); ++y) {
      const auto row =
          shape_.text(v[y], at(p, y), 0, 1000, false, lower_hex_row);
      if (!row) {
        continue;
      }
      for (int x = 0; x < grid_width; ++x) {
        int high = 0;
        int low = 0;
        hex_value((*row)[static_cast<std::size_t>(x) * 2], high);
        hex_value((*row)[(static_cast<std::size_t>(x) * 2) + 1], low);
        def.tiles[(y * grid_width) + static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>((high << 4) | low);
      }
    }
  }

  void districts(const json& v, LocationDefinition& def) {
    const std::string p = "/districts";
    if (!shape_.array(v, p, 0, max_districts)) {
      return;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
      const json& d = v[i];
      const std::string dp = at(p, i);
      shape_.object(d, dp, {"legacy_bytes"},
                    {"id", "name", "number", "type", "traffic_intensity",
                     "crime_stats", "control_screen_alternate_color",
                     "demographic_factor", "demographic_base",
                     "average_income_factor", "service_speed_sensitivity",
                     "min_standard_pizzas", "min_user_pizzas",
                     "average_premise_size", "average_size_modifier",
                     "price_level", "rent_level", "premises_availability",
                     "population_weights_milli"});
      if (!d.is_object()) {
        continue;
      }
      DistrictDefinition out;
      const auto u8 = [&](std::string_view key) {
        return static_cast<int>(
            shape_.integer_at(d, dp, key, 0, 255).value_or(0));
      };
      const auto u16 = [&](std::string_view key) {
        return static_cast<int>(
            shape_.integer_at(d, dp, key, 0, 65535).value_or(0));
      };
      out.id = shape_.text_at(d, dp, "id", 1, 48, false, patterns::entity_id)
                   .value_or("");
      out.name = shape_.text_at(d, dp, "name", 1, 23, true).value_or("");
      out.number = static_cast<int>(
          shape_.integer_at(d, dp, "number", 1, 255).value_or(1));
      if (auto kind = shape_.choice_at(d, dp, "type",
                                       {"residential", "industrial", "business",
                                        "leisure", "shopping", "traffic"})) {
        out.kind = static_cast<DistrictKind>(static_cast<int>(*kind) + 1);
      }
      out.traffic_intensity = u8("traffic_intensity");
      out.crime_stats = u8("crime_stats");
      out.control_screen_alternate_color =
          shape_.boolean_at(d, dp, "control_screen_alternate_color")
              .value_or(false);
      out.demographic_factor = u8("demographic_factor");
      out.demographic_base = u8("demographic_base");
      out.average_income_factor = u8("average_income_factor");
      out.service_speed_sensitivity = u8("service_speed_sensitivity");
      out.min_standard_pizzas = static_cast<int>(
          shape_.integer_at(d, dp, "min_standard_pizzas", 0, 25).value_or(0));
      out.min_user_pizzas = static_cast<int>(
          shape_.integer_at(d, dp, "min_user_pizzas", 0, 15).value_or(0));
      out.average_premise_size = u16("average_premise_size");
      out.average_size_modifier = u16("average_size_modifier");
      out.price_level = u16("price_level");
      out.rent_level = u16("rent_level");
      out.premises_availability = static_cast<int>(
          shape_.integer_at(d, dp, "premises_availability", 0, 100)
              .value_or(0));
      if (const json* weights =
              JsonShape::member(d, "population_weights_milli")) {
        const std::string wp = at(dp, "population_weights_milli");
        if (shape_.array(*weights, wp, population_groups, population_groups)) {
          for (std::size_t k = 0; k < population_groups; ++k) {
            out.population_weights_milli[k] = static_cast<int>(
                shape_.integer((*weights)[k], at(wp, k), 0, 1000000)
                    .value_or(0));
          }
        }
      }
      if (auto legacy = shape_.text_at(d, dp, "legacy_bytes", 0, 208)) {
        int ignored = 0;
        for (const char c : *legacy) {
          if (!hex_value(c, ignored)) {
            shape_.fail(at(dp, "legacy_bytes"), "does not match the pattern");
            break;
          }
        }
      }
      def.districts.push_back(std::move(out));
    }
  }

  void district_cells(const json& v, LocationDefinition& def) {
    const std::string p = "/district_cells";
    if (!shape_.array(v, p, cell_rows, cell_rows)) {
      return;
    }
    for (std::size_t r = 0; r < v.size(); ++r) {
      const auto row = shape_.text(v[r], at(p, r), 0, 100, false, cell_row);
      if (!row) {
        continue;
      }
      for (int c = 0; c < cell_columns; ++c) {
        int value = 0;
        hex_value((*row)[(static_cast<std::size_t>(c) * 2) + 1], value);
        def.district_cells[(r * cell_columns) + static_cast<std::size_t>(c)] =
            static_cast<std::uint8_t>(value);
      }
    }
  }

  void properties(const json& v, LocationDefinition& def) {
    const std::string p = "/properties";
    if (!shape_.array(v, p, 0, 512)) {
      return;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
      const json& item = v[i];
      const std::string ip = at(p, i);
      shape_.object(item, ip, {"label"},
                    {"id", "kind", "tile", "street_side", "interior", "kitchen",
                     "entrance_offset", "kitchen_entry_offset", "realtor",
                     "initial_status", "purchase_price_cents"});
      if (!item.is_object()) {
        continue;
      }
      PropertyDefinition out;
      out.id = shape_.text_at(item, ip, "id", 1, 48, false, patterns::entity_id)
                   .value_or("");
      shape_.text_at(item, ip, "label", 0, 24, true);
      if (auto kind = shape_.choice_at(item, ip, "kind",
                                       {"business_premise", "depot"})) {
        out.kind =
            *kind == 0 ? PropertyType::BUSINESS_PREMISE : PropertyType::DEPOT;
      }
      if (const json* tile = JsonShape::member(item, "tile")) {
        const std::string tp = at(ip, "tile");
        shape_.object(*tile, tp, {}, {"x", "y"});
        out.x = static_cast<int>(
            shape_.integer_at(*tile, tp, "x", 0, grid_width - 1).value_or(0));
        out.y = static_cast<int>(
            shape_.integer_at(*tile, tp, "y", 0, grid_height - 1).value_or(0));
      }
      if (auto side =
              shape_.choice_at(item, ip, "street_side", {"n", "e", "s", "w"})) {
        out.street_side = static_cast<StreetSide>(*side);
      }
      const auto size = [&](std::string_view key, int min_w, int max_w,
                            int min_h, int max_h, int& w, int& h) {
        const json* box = JsonShape::member(item, key);
        if (!box) {
          return;
        }
        const std::string bp = at(ip, key);
        shape_.object(*box, bp, {}, {"w", "h"});
        w = static_cast<int>(
            shape_.integer_at(*box, bp, "w", min_w, max_w).value_or(min_w));
        h = static_cast<int>(
            shape_.integer_at(*box, bp, "h", min_h, max_h).value_or(min_h));
      };
      size("interior", 10, 20, 10, 20, out.interior_w, out.interior_h);
      size("kitchen", 12, 20, 7, 20, out.kitchen_w, out.kitchen_h);
      out.entrance_offset = static_cast<int>(
          shape_.integer_at(item, ip, "entrance_offset", 1, 18).value_or(1));
      out.kitchen_entry_offset = static_cast<int>(
          shape_.integer_at(item, ip, "kitchen_entry_offset", 0, 13)
              .value_or(0));
      out.realtor =
          shape_.text_at(item, ip, "realtor", 1, 48, false, patterns::entity_id)
              .value_or("");
      if (auto status = shape_.choice_at(
              item, ip, "initial_status",
              {"not_available", "for_sale", "for_rent", "for_sale_rent"})) {
        out.initial_status = static_cast<PremiseStatus>(*status);
      }
      out.purchase_price_cents =
          shape_.integer_at(item, ip, "purchase_price_cents", 0, 2147483647)
              .value_or(0);
      def.properties.push_back(std::move(out));
    }
  }

  enum class Role {
    Realtor,
    Bank,
    Insurer,
    Plain,
    Underworld,
  };

  std::optional<ActorDefinition> actor(const json& v, Ptr p, Role role) {
    switch (role) {
    case Role::Realtor:
      shape_.object(v, p, {"unknown_1", "unknown_2"},
                    {"id", "name", "head", "phone", "rip_off_chance"});
      break;
    case Role::Bank:
      shape_.object(v, p, {},
                    {"id", "name", "head", "phone", "interest_rate_modifier",
                     "loan_amount_multiplier"});
      break;
    case Role::Insurer:
      shape_.object(v, p, {},
                    {"id", "name", "head", "phone", "premium_factor",
                     "willingness_threshold", "rejection_percentage"});
      break;
    case Role::Plain:
      shape_.object(v, p, {}, {"id", "name", "head", "phone"});
      break;
    case Role::Underworld:
      shape_.object(v, p, {"unknown_1", "unknown_2", "unknown_3"},
                    {"id", "name", "head", "phone", "crime_danger_level"});
      break;
    }
    if (!v.is_object()) {
      return std::nullopt;
    }
    ActorDefinition out;
    const auto u8 = [&](std::string_view key) {
      return static_cast<int>(shape_.integer_at(v, p, key, 0, 255).value_or(0));
    };
    out.id = shape_.text_at(v, p, "id", 1, 48, false, patterns::entity_id)
                 .value_or("");
    out.name = shape_.text_at(v, p, "name", 1, 23, true).value_or("");
    out.head =
        static_cast<int>(shape_.integer_at(v, p, "head", 1, 25).value_or(1));
    out.phone = static_cast<std::uint32_t>(
        shape_.integer_at(v, p, "phone", 0, 4294967295LL).value_or(0));
    switch (role) {
    case Role::Realtor:
      out.rip_off_chance = u8("rip_off_chance");
      out.unknown_1 = u8("unknown_1");
      out.unknown_2 = u8("unknown_2");
      break;
    case Role::Bank:
      out.interest_rate_modifier = u8("interest_rate_modifier");
      out.loan_amount_multiplier = u8("loan_amount_multiplier");
      break;
    case Role::Insurer:
      out.premium_factor = u8("premium_factor");
      out.willingness_threshold = u8("willingness_threshold");
      out.rejection_percentage = static_cast<int>(
          shape_.integer_at(v, p, "rejection_percentage", 0, 100).value_or(0));
      break;
    case Role::Plain:
      break;
    case Role::Underworld:
      out.crime_danger_level = u8("crime_danger_level");
      out.unknown_1 = u8("unknown_1");
      out.unknown_2 = u8("unknown_2");
      out.unknown_3 = u8("unknown_3");
      break;
    }
    return out;
  }

  void actor_list(const json& actors, Ptr p, std::string_view key,
                  std::size_t max_items, Role role,
                  std::vector<ActorDefinition>& out) {
    const json* list = JsonShape::member(actors, key);
    if (!list) {
      return;
    }
    const std::string lp = at(p, key);
    if (!shape_.array(*list, lp, 0, max_items)) {
      return;
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
      if (auto a = actor((*list)[i], at(lp, i), role)) {
        out.push_back(std::move(*a));
      }
    }
  }

  void actors(const json& v, LocationDefinition& def) {
    const std::string p = "/actors";
    shape_.object(v, p, {},
                  {"realtors", "banks", "insurers", "mayor", "police", "mafia",
                   "arms_dealers"});
    if (!v.is_object()) {
      return;
    }
    auto& a = def.actors;
    actor_list(v, p, "realtors", 10, Role::Realtor, a.realtors);
    actor_list(v, p, "banks", 10, Role::Bank, a.banks);
    actor_list(v, p, "insurers", 10, Role::Insurer, a.insurers);
    if (const json* mayor = JsonShape::member(v, "mayor");
        mayor && !mayor->is_null()) {
      a.mayor = actor(*mayor, at(p, "mayor"), Role::Plain);
    }
    actor_list(v, p, "police", 9, Role::Plain, a.police);
    actor_list(v, p, "mafia", 2, Role::Underworld, a.mafia);
    actor_list(v, p, "arms_dealers", 8, Role::Underworld, a.arms_dealers);
  }

  void suppliers(const json& v, LocationDefinition& def) {
    const std::string p = "/food_suppliers";
    if (!shape_.array(v, p, 0, 32)) {
      return;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
      const json& s = v[i];
      const std::string sp = at(p, i);
      shape_.object(s, sp, {},
                    {"id", "number", "category", "name", "quality",
                     "price_factor", "head", "minimum_contract_dollars",
                     "loyalty_level_dollars", "price_levels"});
      if (!s.is_object()) {
        continue;
      }
      SupplierDefinition out;
      out.id = shape_.text_at(s, sp, "id", 1, 48, false, patterns::entity_id)
                   .value_or("");
      out.number = static_cast<int>(
          shape_.integer_at(s, sp, "number", 0, 255).value_or(0));
      if (auto category = shape_.choice_at(
              s, sp, "category", {"vegetables", "meat", "fruit", "cheese"})) {
        out.category = static_cast<IngredientCategory>(*category);
      }
      out.name = shape_.text_at(s, sp, "name", 1, 25, true).value_or("");
      out.quality = static_cast<int>(
          shape_.integer_at(s, sp, "quality", 0, 255).value_or(0));
      out.price_factor = static_cast<int>(
          shape_.integer_at(s, sp, "price_factor", 0, 255).value_or(0));
      out.head =
          static_cast<int>(shape_.integer_at(s, sp, "head", 1, 25).value_or(1));
      out.minimum_contract_dollars = static_cast<int>(
          shape_.integer_at(s, sp, "minimum_contract_dollars", 0, 65535)
              .value_or(0));
      out.loyalty_level_dollars =
          shape_.integer_at(s, sp, "loyalty_level_dollars", 0, 4294967295LL)
              .value_or(0);
      if (const json* levels = JsonShape::member(s, "price_levels")) {
        const std::string lp = at(sp, "price_levels");
        if (shape_.array(*levels, lp, supplier_price_levels,
                         supplier_price_levels)) {
          for (std::size_t k = 0; k < supplier_price_levels; ++k) {
            out.price_levels[k] = static_cast<std::uint8_t>(
                shape_.integer((*levels)[k], at(lp, k), 0, 255).value_or(0));
          }
        }
      }
      def.food_suppliers.push_back(std::move(out));
    }
  }

  void base_prices(const json& v, LocationDefinition& def) {
    const std::string p = "/ingredient_base_prices";
    if (!shape_.array(v, p, ingredient_price_count, ingredient_price_count)) {
      return;
    }
    for (std::size_t i = 0; i < ingredient_price_count; ++i) {
      def.ingredient_base_prices[i] =
          shape_.integer(v[i], at(p, i), 0, 65535).value_or(0);
    }
  }

  void building_names(const json& v, LocationDefinition& def) {
    const std::string p = "/building_names";
    static constexpr std::array<std::string_view, named_building_types> types =
        {"church", "bank", "cinema",           "school",
         "disco",  "bar",  "department_store", "hotel"};
    shape_.object(v, p, {},
                  {"church", "bank", "cinema", "school", "disco", "bar",
                   "department_store", "hotel"});
    for (std::size_t t = 0; t < types.size(); ++t) {
      const json* list = JsonShape::member(v, types[t]);
      if (!list) {
        continue;
      }
      const std::string lp = at(p, types[t]);
      if (!shape_.array(*list, lp, 0, 24)) {
        continue;
      }
      for (std::size_t i = 0; i < list->size(); ++i) {
        if (auto name = shape_.text((*list)[i], at(lp, i), 1, 15, true)) {
          def.building_names[t].push_back(std::move(*name));
        }
      }
    }
  }

  void template_ref(const json& v, Ptr p) {
    shape_.object(v, p, {}, {"id", "revision", "sha256"});
    shape_.text_at(v, p, "id", 3, 64, false, patterns::dotted_id);
    shape_.integer_at(v, p, "revision", 1,
                      std::numeric_limits<std::int64_t>::max());
    shape_.text_at(v, p, "sha256", 64, 64, false, patterns::sha256);
  }

  void economy(const json& v, LocationDefinition& def) {
    const std::string p = "/economy";
    shape_.object(v, p, {"template", "difficulty_hint"}, {"startup"});
    if (const json* startup = JsonShape::member(v, "startup")) {
      const std::string sp = at(p, "startup");
      shape_.object(*startup, sp, {},
                    {"reference_cash_cents", "min_affordable_premises"});
      def.reference_cash_cents =
          shape_.integer_at(*startup, sp, "reference_cash_cents", 1, 2147483647)
              .value_or(0);
      def.min_affordable_premises = static_cast<int>(
          shape_.integer_at(*startup, sp, "min_affordable_premises", 1, 50)
              .value_or(1));
    }
    if (const json* t = JsonShape::member(v, "template"); t && !t->is_null()) {
      template_ref(*t, at(p, "template"));
    }
    shape_.choice_at(v, p, "difficulty_hint", {"easy", "normal", "hard"});
  }

  void binding(const json& v, Ptr p, std::string& key_out) {
    shape_.object(v, p, {}, {"key", "fallback"});
    key_out =
        shape_.text_at(v, p, "key", 1, 300, false, asset_key).value_or("");
    shape_.choice_at(v, p, "fallback", {"classic", "placeholder"});
  }

  void assets(const json& v, LocationDefinition& def) {
    const std::string p = "/assets";
    shape_.object(v, p, {"kit", "thumbnail"},
                  {"tile_atlas", "medium_atlas", "attachments"});
    if (!v.is_object()) {
      return;
    }
    if (const json* kit = JsonShape::member(v, "kit"); kit && !kit->is_null()) {
      const std::string kp = at(p, "kit");
      shape_.object(*kit, kp, {}, {"kit_id", "revision"});
      shape_.text_at(*kit, kp, "kit_id", 6, 52, false, kit_id);
      shape_.integer_at(*kit, kp, "revision", 1,
                        std::numeric_limits<std::int64_t>::max());
    }
    if (const json* b = JsonShape::member(v, "tile_atlas")) {
      binding(*b, at(p, "tile_atlas"), def.tile_atlas);
    }
    if (const json* b = JsonShape::member(v, "medium_atlas")) {
      binding(*b, at(p, "medium_atlas"), def.medium_atlas);
    }
    if (const json* t = JsonShape::member(v, "thumbnail"); t && !t->is_null()) {
      const std::string tp = at(p, "thumbnail");
      shape_.object(*t, tp, {}, {"attachment"});
      def.thumbnail = shape_.text_at(*t, tp, "attachment", 1, 48, false,
                                     patterns::entity_id);
    }
    const json* list = JsonShape::member(v, "attachments");
    if (!list) {
      return;
    }
    const std::string lp = at(p, "attachments");
    if (!shape_.array(*list, lp, 0, 64)) {
      return;
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
      const json& a = (*list)[i];
      const std::string ap = at(lp, i);
      shape_.object(a, ap, {}, {"id", "role", "file", "state", "source"});
      if (!a.is_object()) {
        continue;
      }
      AttachmentDefinition out;
      out.id = shape_.text_at(a, ap, "id", 1, 48, false, patterns::entity_id)
                   .value_or("");
      shape_.choice_at(a, ap, "role",
                       {"thumbnail", "overview-marker", "tile-hd-overlay",
                        "storefront-hd", "concept"});
      if (const json* file = JsonShape::member(a, "file")) {
        const std::string fp = at(ap, "file");
        shape_.object(*file, fp, {},
                      {"path", "sha256", "bytes", "width", "height"});
        out.path = shape_
                       .text_at(*file, fp, "path", 1, 200, false,
                                patterns::contained_path)
                       .value_or("");
        shape_.text_at(*file, fp, "sha256", 64, 64, false, patterns::sha256);
        shape_.integer_at(*file, fp, "bytes", 1, 16777216);
        shape_.integer_at(*file, fp, "width", 1, 4096);
        shape_.integer_at(*file, fp, "height", 1, 4096);
      }
      if (auto state = shape_.choice_at(
              a, ap, "state", {"quarantine", "approved", "rejected"})) {
        out.state = static_cast<AttachmentState>(*state);
      }
      if (const json* source = JsonShape::member(a, "source")) {
        const std::string sp = at(ap, "source");
        shape_.object(*source, sp, {"provider", "job_id"},
                      {"type", "rights_status"});
        shape_.choice_at(
            *source, sp, "type",
            {"generated", "operator_supplied", "open_source", "placeholder"});
        shape_.text_at(*source, sp, "provider", 0, 64);
        shape_.text_at(*source, sp, "job_id", 0, 128);
        shape_.choice_at(*source, sp, "rights_status",
                         {"unknown", "local_only", "redistributable",
                          "authorized", "prohibited"});
      }
      def.attachments.push_back(std::move(out));
    }
  }

  void provenance(const json& v) {
    const std::string p = "/provenance";
    shape_.object(v, p, {"project_id"}, {"origin", "generator", "created_utc"});
    shape_.choice_at(v, p, "origin",
                     {"generated", "manual", "imported", "mixed"});
    shape_.text_at(v, p, "project_id", 3, 128, false, patterns::project_id);
    shape_.text_at(v, p, "created_utc", 20, 20, false, patterns::utc);
    const json* generator = JsonShape::member(v, "generator");
    if (!generator || generator->is_null()) {
      return;
    }
    const std::string gp = at(p, "generator");
    shape_.object(*generator, gp, {"stage_seeds"},
                  {"id", "version", "template", "settings_sha256", "seed"});
    shape_.text_at(*generator, gp, "id", 3, 64, false, patterns::dotted_id);
    shape_.text_at(*generator, gp, "version", 5, 256, false, patterns::semver);
    if (const json* t = JsonShape::member(*generator, "template")) {
      template_ref(*t, at(gp, "template"));
    }
    shape_.text_at(*generator, gp, "settings_sha256", 64, 64, false,
                   patterns::sha256);
    shape_.text_at(*generator, gp, "seed", 1, 20, false, patterns::u64_decimal);
    if (const json* seeds = JsonShape::member(*generator, "stage_seeds")) {
      const std::string sp = at(gp, "stage_seeds");
      if (!seeds->is_object()) {
        shape_.fail(sp, "expected an object");
        return;
      }
      if (seeds->size() > 16) {
        shape_.fail(sp, "more than 16 members");
      }
      for (const auto& [stage, seed] : seeds->items()) {
        const json name = stage;
        shape_.choice(name, at(sp, stage),
                      {"terrain", "roads", "districts", "premises", "actors",
                       "suppliers", "economy", "names", "placement"});
        shape_.text(seed, at(sp, stage), 1, 20, false, patterns::u64_decimal);
      }
    }
  }

  void integrity(const json& v, LocationDefinition& def) {
    const std::string p = "/integrity";
    shape_.object(v, p, {}, {"canonicalization", "definition_sha256"});
    if (const json* c = JsonShape::member(v, "canonicalization")) {
      shape_.constant(*c, at(p, "canonicalization"), "jcs-rfc8785");
    }
    def.declared_sha256 = shape_.text_at(v, p, "definition_sha256", 64, 64,
                                         false, patterns::sha256);
  }

  JsonShape shape_;
};

} // namespace

std::string canonical_json(const nlohmann::json& document) {
  // nlohmann::json keeps object members in a std::map, so dump() writes them
  // sorted by their UTF-8 bytes, which for the ASCII member names of these
  // documents is the RFC 8785 order; strings stay UTF-8, integers are plain.
  return document.dump();
}

std::string definition_sha256(const nlohmann::json& document) {
  if (!document.is_object() || !document.contains("integrity")) {
    return Sha256::hex(canonical_json(document));
  }
  nlohmann::json copy = document;
  copy.erase("integrity");
  return Sha256::hex(canonical_json(copy));
}

LocationReadResult read_location(const nlohmann::json& document) {
  LocationReadResult result;
  LocationParser parser(result.findings);
  result.definition = parser.parse(document);
  return result;
}

LocationReadResult read_location_text(std::string_view text) {
  nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
  if (document.is_discarded()) {
    LocationReadResult result;
    result.findings.push_back(Finding{.rule = "SCHEMA-1",
                                      .severity = Severity::Error,
                                      .pointer = "",
                                      .message = "not a JSON document"});
    return result;
  }
  return read_location(document);
}

LocationReadResult read_location_file(const std::filesystem::path& path) {
  LocationReadResult result;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_file_bytes) {
    result.findings.push_back(
        Finding{.rule = "SCHEMA-1",
                .severity = Severity::Error,
                .pointer = "",
                .message = ec ? "cannot read " + path.string()
                              : path.string() + " is too large"});
    return result;
  }
  std::ifstream in(path, std::ios::binary);
  std::string text{std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>()};
  if (!in.good() && !in.eof()) {
    result.findings.push_back(
        Finding{.rule = "SCHEMA-1",
                .severity = Severity::Error,
                .pointer = "",
                .message = "cannot read " + path.string()});
    return result;
  }
  return read_location_text(text);
}

} // namespace core::content
