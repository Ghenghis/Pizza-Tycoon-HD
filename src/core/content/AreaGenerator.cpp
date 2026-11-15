/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/AreaGenerator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <utility>

#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"
#include "core/content/Sha256.h"

namespace core::content {

using nlohmann::json;

namespace {

constexpr int W = 160;
constexpr int H = 120;

// splitmix64: the area's own generator, never a simulation stream.
class AreaRandom {
public:
  explicit AreaRandom(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int below(int n) {
    return n <= 0 ? 0
                  : static_cast<int>(next() % static_cast<std::uint64_t>(n));
  }

private:
  std::uint64_t state_;
};

// The original landsym tiles the North Pole style uses (village revision 3).
constexpr uint8_t GRASS = 0x60, TREES = 0xA0, ICE = 0xF3, YARD = 0xE4,
                  PLAZA = 0x64, STALLS = 0x74, MEADOW = 0x80, LAWN = 0x90,
                  FOUNTAIN = 0x9F, POND_L = 0x43, POND_R = 0x53, HAY = 0xC1,
                  GARDEN = 0xD1;
constexpr std::array<uint8_t, 8> TREE_TILES = {0x00, 0x10, 0x20, 0x30,
                                               0x40, 0x50, 0x60, 0x70};
constexpr std::array<uint8_t, 5> SHOPFRONTS = {0xAB, 0xBB, 0xDB, 0xFB, 0xCB};
constexpr uint8_t ROOF_START = 0x14, ROOF_MID = 0x24, ROOF_END = 0x34;
constexpr std::array<std::pair<uint8_t, uint8_t>, 5> HOUSE_PAIRS = {
    {{0x09, 0x19}, {0x29, 0x39}, {0x49, 0x59}, {0x69, 0x79}, {0x89, 0x99}}};
constexpr uint8_t ROAD_W = 0x06, ROAD_E = 0x16, ROAD_S = 0x26, ROAD_N = 0x36;
constexpr uint8_t SW = 0x46, SE = 0x56, NW = 0x66, NE = 0x76;
constexpr uint8_t C_W = 0x86, C_N = 0x96, C_E = 0xA6, C_S = 0xB6;
constexpr uint8_t ZEBRA_E = 0xD7, ZEBRA_S = 0xE7, SHADOW_ZEBRA_N = 0xC7,
                  SHADOW_ROAD_W = 0x17, SHADOW2_ROAD_E = 0x97;
constexpr uint8_t DEPOT = 0x04;
constexpr std::array<uint8_t, 8> PREMISE_CODES = {0x8A, 0x9A, 0xAA, 0xBA,
                                                  0xCA, 0xDA, 0xEA, 0xFA};
constexpr std::array<uint8_t, 8> NAMED = {0x88, 0x98, 0xA8, 0xB8,
                                          0xC8, 0xD8, 0xE8, 0xF8};
constexpr std::array<std::string_view, 8> STATUSES = {
    "for_rent", "for_sale_rent", "for_rent",      "not_available",
    "for_sale", "for_rent",      "not_available", "for_sale_rent"};
// District names of the inherited kit, by theme in the home location's
// district order (market, workshops, square, meadows, gardens, harbour).
constexpr std::array<std::array<std::string_view, 4>, 6> NAME_POOLS = {{
    {"Snowdrift Market", "Tinsel Bazaar", "Peppermint Market",
     "Gingerbread Row"},
    {"Tinsel Works", "Sleighbell Foundry", "Ribbon Mill Quarter",
     "Toymakers Yard"},
    {"Lantern Square", "Starlight Plaza", "Hollyberry Square", "Carol Square"},
    {"Antler Hollow", "Sledhill Meadows", "Moonlit Paddocks", "Elkwood Park"},
    {"Icicle Gardens", "Mistletoe Lanes", "Cocoa Cottages", "Pinecone Terrace"},
    {"Glacier Wharf", "Northern Lights Quay", "Frostbay Docks", "Polar Inlet"},
}};
constexpr std::string_view kit_id = "kit:north-pole-village";
constexpr std::string_view generator_id = "pthd.area-generator";
constexpr std::string_view generator_version = "1.0.0";

using Map = std::array<std::array<uint8_t, W>, H>;

void fill(Map& m, int x0, int y0, int x1, int y1, uint8_t code) {
  for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y) {
    for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
      m[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = code;
    }
  }
}

uint8_t& at(Map& m, int x, int y) {
  return m[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
}

std::string hex2(uint8_t v) {
  constexpr std::string_view digits = "0123456789abcdef";
  return {digits[v >> 4], digits[v & 0x0F]};
}

std::string two_digits(int v) {
  char text[8];
  std::snprintf(text, sizeof text, "%02d", v);
  return text;
}

Finding error(std::string rule, std::string message) {
  return Finding{.rule = std::move(rule),
                 .severity = Severity::Error,
                 .pointer = "",
                 .message = std::move(message)};
}

struct Candidate {
  int x;
  int y;
  char side;
};

// The template reference of the home location (provenance and economy).
json template_ref(const LocationDefinition& style, std::string_view territory) {
  return json{{"id", "pthd." + std::string(territory) + ".village"},
              {"revision", style.revision},
              {"sha256", style.sha256}};
}

// The parts every area of the territory shares with its home location:
// actors, suppliers, prices, names, world placement, facets, grid.
json shell(const json& style, std::string_view key, std::string_view name,
           std::string_view territory) {
  json doc;
  doc["format"] = "pthd.location";
  doc["schema_version"] = "1.0.0";
  doc["location_key"] = key;
  doc["revision"] = 1;
  doc["display"] =
      json{{"name", name},
           {"short_name", style.at("display").at("short_name")},
           {"aliases", json::array()},
           {"tags", style.at("display").value("tags", json::array())}};
  doc["world"] = style.at("world");
  doc["facets"] = style.at("facets");
  doc["grid"] = style.at("grid");
  doc["actors"] = style.at("actors");
  doc["food_suppliers"] = style.at("food_suppliers");
  doc["ingredient_base_prices"] = style.at("ingredient_base_prices");
  doc["building_names"] = style.at("building_names");
  const json& economy = style.at("economy");
  doc["economy"] =
      json{{"startup", economy.at("startup")},
           {"template", nullptr},
           {"difficulty_hint", economy.value("difficulty_hint", "normal")}};
  const std::string t(territory);
  doc["assets"] = json{
      {"kit", json{{"kit_id", kit_id}, {"revision", 1}}},
      {"tile_atlas",
       json{{"key", t + ":gfx/landsym"},
            {"fallback", style.at("assets").at("tile_atlas").at("fallback")}}},
      {"medium_atlas",
       json{
           {"key", t + ":gfx/med1"},
           {"fallback", style.at("assets").at("medium_atlas").at("fallback")}}},
      {"thumbnail", nullptr},
      {"attachments", json::array()}};
  return doc;
}

json tiles_json(const Map& m) {
  json rows = json::array();
  for (const auto& row : m) {
    std::string text;
    text.reserve(W * 2);
    for (const uint8_t v : row) {
      text += hex2(v);
    }
    rows.push_back(std::move(text));
  }
  return rows;
}

// The automatic map: the prototype in the studio notes, step for step
// (/tmp/w03/area/gen_area.py was checked against the validator over 200
// seeds before this was written).
void automatic(const json& style, std::uint64_t seed, json& doc) {
  AreaRandom r(seed);
  const int XL = 14 + r.below(9);
  const int XR = 134 + r.below(9);
  const int YT = 14 + r.below(9);
  const int YB = 92 + r.below(7);
  const int XC = 64 + r.below(29);
  const int YM = 50 + r.below(15);

  Map m{};
  for (auto& row : m) {
    row.fill(GRASS);
  }
  // Scenery: forests in three corners.
  fill(m, 0, 0, XL - 3, YT - 3, TREES);
  fill(m, XR + 4, 0, W - 1, YT - 3, TREES);
  fill(m, XR + 4, YB + 4, W - 1, H - 1, TREES);
  // The market: rows of shopfronts and lanes of stalls.
  for (int y = YT + 4; y < YM - 3; ++y) {
    for (int x = XL + 4; x < XC - 5; ++x) {
      at(m, x, y) =
          (y - YT - 4) % 6 == 5
              ? STALLS
              : SHOPFRONTS[static_cast<std::size_t>((x + 2 * (y / 6)) % 5)];
    }
  }
  // The workshops: long red roofs with yards.
  for (int y = YT + 4; y < YM - 3; ++y) {
    for (int x = XC + 6; x < XR - 3; ++x) {
      if ((y - YT - 4) % 4 == 3) {
        at(m, x, y) = YARD;
        continue;
      }
      const int k = (x - XC - 6) % 7;
      at(m, x, y) = k == 6   ? YARD
                    : k == 0 ? ROOF_START
                    : k == 5 ? ROOF_END
                             : ROOF_MID;
    }
  }
  // The meadows: trees, a hay field and ponds.
  fill(m, XL + 4, YM + 5, XC - 6, YB - 4, MEADOW);
  const int seed_mod = static_cast<int>(seed % 9);
  for (int y = YM + 5; y < YB - 3; ++y) {
    for (int x = XL + 4; x < XC - 5; ++x) {
      if (((x * 7 + y * 13) % 9 + seed_mod) % 9 == 0) {
        at(m, x, y) = TREE_TILES[static_cast<std::size_t>((x + y) % 8)];
      }
    }
  }
  const int hx = XL + 6 + r.below(6);
  const int hy = YB - 12 + r.below(4);
  fill(m, hx, hy, hx + 8, hy + 6, HAY);
  for (int i = 0; i < 4; ++i) {
    const int px = XL + 6 + r.below(std::max(1, XC - XL - 16));
    const int py = YM + 6 + r.below(std::max(1, YB - YM - 12));
    at(m, px, py) = POND_L;
    at(m, px + 1, py) = POND_R;
  }
  // The gardens: pairs of houses, garden rows, lawns.
  fill(m, XC + 6, YM + 5, XR - 4, YB - 4, LAWN);
  for (int y = YM + 5; y < YB - 3; ++y) {
    if ((y - YM - 5) % 3 == 0) {
      int i = 0;
      for (int x = XC + 7; x < XR - 5; x += 3) {
        const auto [left, right] =
            HOUSE_PAIRS[static_cast<std::size_t>((i + y) % 5)];
        at(m, x, y) = left;
        at(m, x + 1, y) = right;
        ++i;
      }
    }
    else if ((y - YM - 5) % 3 == 1) {
      for (int x = XC + 6; x < XR - 3; ++x) {
        if ((x / 3) % 2 == 0) {
          at(m, x, y) = GARDEN;
        }
      }
    }
  }
  // The square, with four fountains.
  fill(m, XC - 14, YM - 7, XC + 15, YM + 8, PLAZA);
  at(m, XC - 10, YM - 5) = FOUNTAIN;
  at(m, XC + 11, YM - 5) = FOUNTAIN;
  at(m, XC - 10, YM + 6) = FOUNTAIN;
  at(m, XC + 11, YM + 6) = FOUNTAIN;
  // The harbour, frozen over, with its quay.
  if (YB + 8 <= H - 1) {
    fill(m, XL - 2, YB + 6, XR + 3, YB + 7, YARD);
    fill(m, 0, YB + 8, XR + 3, H - 1, ICE);
  }

  // Roads: an avenue and a street edge to edge, a ring of two lanes.
  for (int x = 0; x < W; ++x) {
    at(m, x, YM) = ROAD_W;
    at(m, x, YM + 1) = ROAD_E;
  }
  for (int y = 0; y < H; ++y) {
    at(m, XC, y) = ROAD_S;
    at(m, XC + 1, y) = ROAD_N;
  }
  for (int x = XL; x < XR + 2; ++x) {
    at(m, x, YT) = ROAD_W;
    at(m, x, YT + 1) = ROAD_E;
    at(m, x, YB) = ROAD_W;
    at(m, x, YB + 1) = ROAD_E;
  }
  for (int y = YT; y < YB + 2; ++y) {
    at(m, XL, y) = ROAD_S;
    at(m, XL + 1, y) = ROAD_N;
    at(m, XR, y) = ROAD_S;
    at(m, XR + 1, y) = ROAD_N;
  }
  at(m, XL, YT) = C_S;
  at(m, XL + 1, YT) = ROAD_W;
  at(m, XL, YT + 1) = ROAD_S;
  at(m, XL + 1, YT + 1) = C_E;
  at(m, XR + 1, YT) = C_W;
  at(m, XR, YT) = ROAD_W;
  at(m, XR, YT + 1) = C_S;
  at(m, XR + 1, YT + 1) = ROAD_N;
  at(m, XL, YB + 1) = C_E;
  at(m, XL, YB) = ROAD_S;
  at(m, XL + 1, YB) = C_N;
  at(m, XL + 1, YB + 1) = ROAD_E;
  at(m, XR + 1, YB + 1) = C_N;
  at(m, XR, YB + 1) = ROAD_E;
  at(m, XR, YB) = C_W;
  at(m, XR + 1, YB) = ROAD_N;
  for (const auto& [b, a] : std::array<std::pair<int, int>, 5>{
           {{XL, YM}, {XR, YM}, {XC, YT}, {XC, YB}, {XC, YM}}}) {
    at(m, b, a) = SW;
    at(m, b + 1, a) = NW;
    at(m, b, a + 1) = SE;
    at(m, b + 1, a + 1) = NE;
  }
  for (const int x : {XC - 3, XC - 2, XC + 3, XC + 4}) {
    at(m, x, YM + 1) = ZEBRA_E;
  }
  for (const int y : {YM - 3, YM - 2, YM + 3, YM + 4}) {
    at(m, XC, y) = ZEBRA_S;
    at(m, XC + 1, y) = SHADOW_ZEBRA_N;
  }
  for (int x = XC + 6; x < XR - 3; ++x) {
    at(m, x, YT) = SHADOW_ROAD_W;
    at(m, x, YT + 1) = SHADOW2_ROAD_E;
  }

  // Premise sites beside the lanes, away from crossings and corners.
  const auto near = [](int v, std::initializer_list<int> centers) {
    return std::any_of(centers.begin(), centers.end(),
                       [v](int c) { return std::abs(v - c) <= 3; });
  };
  std::vector<Candidate> cands;
  for (const auto& [y, side] :
       std::array<std::pair<int, char>, 6>{{{YT - 1, 's'},
                                            {YT + 2, 'n'},
                                            {YM - 1, 's'},
                                            {YM + 2, 'n'},
                                            {YB - 1, 's'},
                                            {YB + 2, 'n'}}}) {
    int x = XL + 4 + r.below(4);
    while (x <= XR - 3) {
      if (!near(x, {XL, XL + 1, XR, XR + 1, XC, XC + 1})) {
        cands.push_back({x, y, side});
      }
      x += 6 + r.below(7);
    }
  }
  for (const auto& [x, side] :
       std::array<std::pair<int, char>, 2>{{{XC - 1, 'e'}, {XC + 2, 'w'}}}) {
    int y = YT + 4 + r.below(4);
    while (y <= YB - 3) {
      if (!near(y, {YT, YT + 1, YM, YM + 1, YB, YB + 1})) {
        cands.push_back({x, y, side});
      }
      y += 6 + r.below(7);
    }
  }
  for (std::size_t i = cands.size() - 1; i > 0; --i) {
    const auto j = static_cast<std::size_t>(r.below(static_cast<int>(i) + 1));
    std::swap(cands[i], cands[j]);
  }
  const int count =
      std::min(static_cast<int>(cands.size()) - 9, 40 + r.below(7));
  std::vector<Candidate> chosen(cands.begin(), cands.begin() + count + 1);
  std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  const std::vector<Candidate> named(cands.begin() + count + 1,
                                     cands.begin() + count + 9);
  const int depot_i = r.below(static_cast<int>(chosen.size()));

  // District cells: outside the ring the harbour, the square around the
  // crossing, then the four quarters.
  const auto district_index = [&](int cx, int cy) {
    if (cx < XL || cx > XR + 1 || cy < YT || cy > YB + 1) {
      return 5;
    }
    if (XC - 14 <= cx && cx <= XC + 15 && YM - 7 <= cy && cy <= YM + 8) {
      return 2;
    }
    if (cy < YM) {
      return cx < XC ? 0 : 1;
    }
    return cx < XC ? 3 : 4;
  };
  std::array<std::array<int, 20>, 15> cells{};
  json cell_rows = json::array();
  for (int row = 0; row < 15; ++row) {
    std::string text;
    for (int col = 0; col < 20; ++col) {
      const int d = district_index(8 * col + 4, 8 * row + 4);
      cells[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = d;
      text += hex2(static_cast<uint8_t>(d));
    }
    cell_rows.push_back(std::move(text));
  }

  // Districts of the inherited kit: the home location's demand and prices,
  // the kit's names.
  json districts = json::array();
  const json& style_districts = style.at("districts");
  for (std::size_t i = 0; i < style_districts.size(); ++i) {
    json d = style_districts[i];
    const std::string name(NAME_POOLS[std::min<std::size_t>(
        i, NAME_POOLS.size() - 1)][static_cast<std::size_t>(r.below(4))]);
    d["name"] = name;
    d["id"] = "d-" + area_slug(name);
    districts.push_back(std::move(d));
  }

  const int status_offset = r.below(8);
  std::vector<std::string> realtors;
  for (const auto& a : style.at("actors").at("realtors")) {
    realtors.push_back(a.at("id").get<std::string>());
  }
  json props = json::array();
  int business = 0;
  for (std::size_t n = 0; n < chosen.size(); ++n) {
    const int i = static_cast<int>(n);
    const auto& c = chosen[n];
    const bool depot = i == depot_i;
    const int w = 12 + (i * 5) % 7;
    const int h = 12 + (i * 3) % 7;
    const int kw = std::min(20, std::max(12, w - 1 + (i % 3)));
    const int kh = w - (i % 4);
    const int eo = 1 + (i * 7) % (w - 2);
    const int keo = (i * 5) % (kh - 6);
    const json& d =
        districts[static_cast<std::size_t>(cells[static_cast<std::size_t>(
            c.y / 8)][static_cast<std::size_t>(c.x / 8)])];
    const std::int64_t price =
        d.at("price_level").get<std::int64_t>() * w * h * 100 * (depot ? 5 : 1);
    std::string id = depot ? "p-depot" : "p-shop-" + two_digits(++business);
    at(m, c.x, c.y) =
        depot ? DEPOT : PREMISE_CODES[static_cast<std::size_t>(i % 8)];
    props.push_back(json{{"id", std::move(id)},
                         {"kind", depot ? "depot" : "business_premise"},
                         {"tile", json{{"x", c.x}, {"y", c.y}}},
                         {"street_side", std::string(1, c.side)},
                         {"interior", json{{"w", w}, {"h", h}}},
                         {"kitchen", json{{"w", kw}, {"h", kh}}},
                         {"entrance_offset", eo},
                         {"kitchen_entry_offset", keo},
                         {"realtor", realtors[n % realtors.size()]},
                         {"initial_status",
                          depot ? std::string("for_sale_rent")
                                : std::string(STATUSES[static_cast<std::size_t>(
                                      (i + status_offset) % 8)])},
                         {"purchase_price_cents", price}});
  }
  for (std::size_t k = 0; k < named.size(); ++k) {
    at(m, named[k].x, named[k].y) = NAMED[k];
  }

  doc["tiles"] = tiles_json(m);
  doc["districts"] = std::move(districts);
  doc["district_cells"] = std::move(cell_rows);
  doc["properties"] = std::move(props);
}

// The empty draft: the style's districts and suppliers on a meadow, no roads
// and no premises.
void empty(const json& style, json& doc) {
  Map m{};
  for (auto& row : m) {
    row.fill(GRASS);
  }
  doc["tiles"] = tiles_json(m);
  doc["districts"] = style.at("districts");
  json rows = json::array();
  for (int row = 0; row < 15; ++row) {
    rows.push_back(std::string(40, '0'));
  }
  doc["district_cells"] = std::move(rows);
  doc["properties"] = json::array();
}

bool latin1_codepoints(std::string_view text, std::vector<unsigned>& out) {
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out.push_back(c);
      ++i;
    }
    else if ((c & 0xE0) == 0xC0 && i + 1 < text.size() &&
             (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
      out.push_back(((c & 0x1Fu) << 6) |
                    (static_cast<unsigned char>(text[i + 1]) & 0x3Fu));
      i += 2;
    }
    else {
      return false; // beyond ISO-8859-1, or not UTF-8
    }
  }
  return true;
}

} // namespace

std::string_view area_mode_name(AreaMode mode) {
  return mode == AreaMode::Automatic ? "auto" : "empty";
}

bool valid_area_name(std::string_view name) {
  std::vector<unsigned> cps;
  if (!latin1_codepoints(name, cps) || cps.empty() || cps.size() > 24) {
    return false;
  }
  return std::all_of(cps.begin(), cps.end(), [](unsigned c) {
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF);
  });
}

std::string area_slug(std::string_view name) {
  std::string out;
  for (const char raw : name) {
    char ch = raw;
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      out += ch;
    }
    else if (!out.empty() && out.back() != '-') {
      out += '-';
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  if (out.size() > 40) {
    out.resize(40);
    while (!out.empty() && out.back() == '-') {
      out.pop_back();
    }
  }
  return out.empty() ? "area" : out;
}

json with_integrity(json document) {
  document.erase("integrity");
  const std::string sha = definition_sha256(document);
  document["integrity"] =
      json{{"canonicalization", "jcs-rfc8785"}, {"definition_sha256", sha}};
  return document;
}

AreaProposal generate_area(const AreaRequest& request) {
  AreaProposal out;
  if (!valid_area_name(request.name)) {
    out.findings.push_back(error(
        "AREA-1", "'" + request.name +
                      "' cannot be an area name: 1 to 24 printable characters "
                      "of the game's character set"));
    return out;
  }
  if (!request.style || request.style->canonical_document.empty()) {
    out.findings.push_back(
        error("AREA-2", "the territory has no home location to take the "
                        "style from"));
    return out;
  }
  const json style = json::parse(request.style->canonical_document);
  const std::string territory =
      request.style->bonus ? request.style->bonus_route : request.style->region;
  if (territory != request.territory) {
    out.findings.push_back(error("AREA-2", request.style->key.str() +
                                               " is not in the territory " +
                                               request.territory));
    return out;
  }
  if (style.at("districts").size() != NAME_POOLS.size()) {
    out.findings.push_back(
        error("AREA-2",
              "the style has " + std::to_string(style.at("districts").size()) +
                  " districts; the " + std::string(kit_id) + " kit has six"));
    return out;
  }

  // A fresh key: <namespace>:<territory>-<slug>, numbered on a clash.
  const std::string base = territory + "-" + area_slug(request.name);
  std::string key;
  for (int n = 1; n < 1000; ++n) {
    std::string local = n == 1 ? base : base + "-" + std::to_string(n);
    if (local.size() > 63) {
      local = base.substr(0, 63 - (n == 1 ? 0 : std::to_string(n).size() + 1));
      while (!local.empty() && local.back() == '-') {
        local.pop_back();
      }
      if (n > 1) {
        local += "-" + std::to_string(n);
      }
    }
    const std::string candidate = request.key_namespace + ":" + local;
    if (!request.taken_keys.contains(candidate) &&
        candidate != request.style->key.str()) {
      key = candidate;
      break;
    }
  }
  if (key.empty() || !ContentKey::parse(key)) {
    out.findings.push_back(error("AREA-3", "no free location key for '" +
                                               request.name + "' in " +
                                               request.key_namespace));
    return out;
  }
  std::string document_id = key;
  std::replace(document_id.begin(), document_id.end(), ':', '.');

  json doc = shell(style, key, request.name, territory);
  const json tref = template_ref(*request.style, territory);
  doc["economy"]["template"] = tref;
  const json settings = json{{"territory", territory},
                             {"name", request.name},
                             {"mode", area_mode_name(request.mode)},
                             {"seed", std::to_string(request.seed)},
                             {"template", tref}};
  doc["provenance"] = json{
      {"origin", request.mode == AreaMode::Automatic ? "generated" : "manual"},
      {"project_id", document_id},
      {"generator",
       json{{"id", generator_id},
            {"version", generator_version},
            {"template", tref},
            {"settings_sha256", Sha256::hex(canonical_json(settings))},
            {"seed", std::to_string(request.seed)}}},
      {"created_utc", request.created_utc}};

  if (request.mode == AreaMode::Empty) {
    empty(style, doc);
    out.document = std::move(doc);
    out.location_key = key;
    out.document_id = document_id;
    return out;
  }

  automatic(style, request.seed, doc);
  doc = with_integrity(std::move(doc));
  // Proposed only when the game would admit it.
  auto read = read_location(doc);
  out.findings = read.findings;
  if (read.definition) {
    const auto semantic = validate_location(*read.definition);
    out.findings.insert(out.findings.end(), semantic.begin(), semantic.end());
  }
  if (!read.definition || has_errors(out.findings)) {
    out.findings.push_back(
        error("AREA-4", "the generated area did not pass the location rules; "
                        "nothing was added"));
    return out;
  }
  out.document = std::move(doc);
  out.location_key = key;
  out.document_id = document_id;
  return out;
}

std::vector<std::string> missing_requirements(const json& document) {
  auto read = read_location(document);
  Findings findings = read.findings;
  if (read.definition) {
    const auto semantic = validate_location(*read.definition);
    findings.insert(findings.end(), semantic.begin(), semantic.end());
  }
  // In words a player can act on, by rule; once per rule.
  static const std::map<std::string, std::string, std::less<>> words = {
      {"ROAD-1", "Lay a road network: the map has no roads."},
      {"ROAD-2", "Fix the roads that end or turn back on themselves."},
      {"ROAD-3", "Join every road into one connected network."},
      {"ROAD-4", "Give every one-way lane its return lane."},
      {"PROP-1", "Give every premise tile exactly one property."},
      {"PROP-2", "Fit each premise's entrance and kitchen door to its size."},
      {"PROP-3", "Put every premise beside a road of the network."},
      {"PROP-5",
       "Add business premises: the computer players need at least 16."},
      {"ECON-1", "Make enough premises affordable with the starting money."},
      {"ECON-2", "Lower the price levels that overflow."},
      {"DIST-1", "Add at least one district."},
      {"DIST-3", "Give every district a population."},
      {"REF-1", "Give every property a realtor of this area."},
      {"REF-2", "Paint every district cell with an existing district."},
      {"SUP-1", "Add a food supplier for every ingredient category."},
      {"SUP-3", "Price every ingredient."},
      {"ACT-1", "Complete the cast: realtors, banks, insurers, police and a "
                "mayor."},
      {"TILE-1", "Use only free shop fronts for premises."},
  };
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& f : findings) {
    if (f.severity != Severity::Error || !seen.insert(f.rule).second) {
      continue;
    }
    if (const auto it = words.find(f.rule); it != words.end()) {
      out.push_back(it->second);
    }
    else {
      out.push_back(f.rule + ": " + f.message +
                    (f.pointer.empty() ? "" : " (" + f.pointer + ")"));
    }
  }
  return out;
}

json renamed_document(const json& document, std::string_view name) {
  json doc = document;
  doc["display"]["name"] = name;
  doc["revision"] = doc.value("revision", 1) + 1;
  doc.erase("integrity");
  return doc;
}

} // namespace core::content
