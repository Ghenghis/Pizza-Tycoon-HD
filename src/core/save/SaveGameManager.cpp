/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/SaveGameManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <cstring>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <zlib.h>
// zlib.h defines FAR as a macro for legacy DOS far-pointer compat, which
// conflicts with our ZoomLevel::FAR
#undef FAR

#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/MayorOffice.h"
#include "core/city/Property.h"
#include "core/economy/WeaponMarket.h"
#include "core/events/CareerLog.h"
#include "core/events/MilestoneId.h"
#include "core/food/IngredientCategoryType.h"
#include "core/food/PizzaJudging.h"
#include "core/init/NewGameConfig.h"
#include "core/mission/MissionTarget.h"
#include "core/restaurant/Restaurant.h"
#include "core/restaurant/StaffEventData.h"
#include "core/save/SaveGameCharacter.h"
#include "core/save/SaveGameCity.h"
#include "core/save/SaveGameDate.h"
#include "core/save/SaveGameFood.h"
#include "core/save/SaveGameRestaurant.h"
#include "core/state/GameOptions.h"
#include "core/state/GameRules.h"
#include "core/state/GameState.h"
#include "core/state/TurnOrchestrator.h"
#include "core/time/GameTime.h"
#include "core/util/AtomicFile.h"
#include "core/util/GameContext.h"
#include "engine/Endian.h"
#include "engine/FormatCompat.h"
#include "engine/Logger.h"
#include "engine/PlatformPaths.h"
#include "engine/Random.h"
#include "engine/StringUtil.h"

namespace save {

namespace fs = std::filesystem;

// Save file layout: SaveHeader, then (version >= 2) a uint32 length followed
// by that many bytes of uncompressed metadata JSON, then the zlib payload.
struct SaveHeader {
  char magic[6];    // "PLSAVE"
  uint16_t version; // little-endian
  uint64_t size;    // uncompressed payload size, little-endian
};
static_assert(sizeof(SaveHeader) == 16);

constexpr uint16_t SAVE_HEADER_VERSION = 2;
constexpr uint16_t HIGH_SCORE_HEADER_VERSION = 1;
constexpr uint32_t MAX_METADATA_SIZE = 1024 * 1024;

// The metadata block is a handful of fields.
constexpr int MAX_METADATA_DEPTH = 8;
constexpr std::uint64_t MAX_METADATA_VALUES = 4096;

// A high score file holds eight names and scores.
constexpr std::uint64_t MAX_HIGH_SCORE_FILE_SIZE = 1024 * 1024;
constexpr std::uint64_t MAX_HIGH_SCORE_JSON_SIZE = 1024 * 1024;
constexpr int MAX_HIGH_SCORE_DEPTH = 8;
constexpr std::uint64_t MAX_HIGH_SCORE_VALUES = 4096;

// zlib takes sizes as uLong, which is 32 bits on Windows.
static_assert(MAX_SAVE_FILE_SIZE <= std::numeric_limits<uLong>::max());
static_assert(MAX_SAVE_JSON_SIZE <= std::numeric_limits<uLong>::max());

// What the player is told. The log gets the details.
constexpr std::string_view save_failed_message =
    "The game could not be saved. No save file was changed.";
constexpr std::string_view save_disk_full_message =
    "The game could not be saved because the disk is full. No save file was "
    "changed.";
constexpr std::string_view save_no_access_message =
    "The game could not be saved because the save folder cannot be written "
    "to. No save file was changed.";
constexpr std::string_view load_unreadable_message =
    "The save file could not be read.";
constexpr std::string_view load_damaged_message =
    "This save file is damaged and cannot be loaded.";
constexpr std::string_view load_newer_message =
    "This save was made by a newer version of the game and cannot be loaded.";
// Only if building the game fails after the same build passed on a scratch
// state, which the checks make next to impossible.
constexpr std::string_view load_failed_message =
    "The save could not be loaded, and the game in progress could not be "
    "kept. Please restart the game.";

namespace {

void set_message(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = message;
  }
}

// A path as UTF-8, for the log.
std::string display(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::string_view save_failure_message(std::error_code code) {
  if (code == std::errc::no_space_on_device) {
    return save_disk_full_message;
  }
  if (code == std::errc::permission_denied ||
      code == std::errc::operation_not_permitted ||
      code == std::errc::read_only_file_system) {
    return save_no_access_message;
  }
  return save_failed_message;
}

// Creates the folder a file goes in, if it isn't there yet.
std::error_code create_parent_folder(const fs::path& path) {
  std::error_code ec;
  const fs::path parent = path.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
  }
  return ec;
}

// Why a save could not be used: details for the log, a message for the player.
struct LoadProblem {
  std::string detail;
  std::string_view message = load_damaged_message;
};

// Thrown while parsing a document that goes past a limit.
class LimitExceeded : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Parses JSON, stopping as soon as the document nests deeper than max_depth
// or holds more than max_values values, before they take up the memory.
json parse_bounded(std::string_view text, int max_depth,
                   std::uint64_t max_values) {
  std::uint64_t values = 0;
  const json::parser_callback_t check =
      [&values, max_depth, max_values](int depth, json::parse_event_t event,
                                       json& /*parsed*/) {
        if (depth > max_depth) {
          throw LimitExceeded("JSON nested more than " +
                              std::to_string(max_depth) + " deep");
        }
        if (event == json::parse_event_t::object_start ||
            event == json::parse_event_t::array_start ||
            event == json::parse_event_t::value) {
          if (++values > max_values) {
            throw LimitExceeded("JSON with more than " +
                                std::to_string(max_values) + " values");
          }
        }
        return true;
      };
  return json::parse(text.data(), text.data() + text.size(), check);
}

struct DocumentSize {
  std::uint64_t values = 0;
  int depth = 0;
};

// Counts values and nesting the way parse_bounded does: the document itself
// is at depth 0, what it holds at depth 1, and so on.
void measure(const json& value, int depth, DocumentSize& size) {
  ++size.values;
  size.depth = std::max(size.depth, depth);
  if (value.is_structured()) {
    for (const json& child : value) {
      measure(child, depth + 1, size);
    }
  }
}

// Extract the load-menu metadata fields from a full save document.
json metadata_from_save_json(const json& j) {
  json meta;
  meta["saved_at"] = j.value("saved_at", "Unknown");
  if (j.contains("time") && j.at("time").contains("display")) {
    meta["game_date"] = j.at("time").at("display");
  }
  json names = json::array();
  if (j.contains("players")) {
    for (const auto& p : j.at("players")) {
      names.push_back(p.is_null() ? "" : p.value("name", ""));
    }
  }
  meta["player_names"] = std::move(names);
  meta["active_player"] = j.value("active_player", -1);
  meta["active_city"] = j.value("active_city", -1);
  return meta;
}

// The bytes of a save file holding this document. Throws if the save would
// break a limit the loader enforces: the game never writes a save it would
// refuse to load.
std::string encode_save(const json& j) {
  DocumentSize size;
  measure(j, 0, size);
  if (size.values > MAX_SAVE_JSON_VALUES || size.depth > MAX_SAVE_JSON_DEPTH) {
    throw std::length_error("the game holds " + std::to_string(size.values) +
                            " values nested " + std::to_string(size.depth) +
                            " deep, past what a save may hold");
  }

  const std::string json_str = j.dump(-1);
  if (json_str.size() > MAX_SAVE_JSON_SIZE) {
    throw std::length_error("the game data is " +
                            std::to_string(json_str.size()) +
                            " bytes, past what a save may hold");
  }

  const std::string meta_str = metadata_from_save_json(j).dump(-1);
  if (meta_str.size() > MAX_METADATA_SIZE) {
    throw std::length_error("the save metadata is " +
                            std::to_string(meta_str.size()) +
                            " bytes, past what a save may hold");
  }

  const size_t payload_offset =
      sizeof(SaveHeader) + sizeof(uint32_t) + meta_str.size();

  uLongf bound = compressBound(static_cast<uLong>(json_str.size()));
  std::string out(payload_offset + bound, '\0');
  uLongf compressed_size = bound;
  int zret = compress2(
      reinterpret_cast<Bytef*>(out.data() + payload_offset), &compressed_size,
      reinterpret_cast<const Bytef*>(json_str.data()),
      static_cast<uLong>(json_str.size()), Z_DEFAULT_COMPRESSION);
  if (zret != Z_OK) {
    throw std::runtime_error("compression failed (zlib error " +
                             std::to_string(zret) + ")");
  }
  out.resize(payload_offset + compressed_size);
  if (out.size() > MAX_SAVE_FILE_SIZE) {
    throw std::length_error("the save would be " + std::to_string(out.size()) +
                            " bytes, past what a save may be");
  }

  SaveHeader hdr;
  std::memcpy(hdr.magic, "PLSAVE", 6);
  hdr.version = endian::to_le16(SAVE_HEADER_VERSION);
  hdr.size = endian::to_le64(static_cast<uint64_t>(json_str.size()));
  std::memcpy(out.data(), &hdr, sizeof(hdr));

  const uint32_t meta_len =
      endian::to_le32(static_cast<uint32_t>(meta_str.size()));
  std::memcpy(out.data() + sizeof(SaveHeader), &meta_len, sizeof(meta_len));
  std::memcpy(out.data() + sizeof(SaveHeader) + sizeof(uint32_t),
              meta_str.data(), meta_str.size());
  return out;
}

// Reads a save file: checks the header and every size in it before
// allocating for them, then inflates and parses the game data.
std::optional<json> decode_save_file(const fs::path& path,
                                     LoadProblem& problem) {
  const std::string name = display(path);
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    problem = {"cannot open " + name, load_unreadable_message};
    return std::nullopt;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff end = file.tellg();
  file.seekg(0, std::ios::beg);
  if (!file.good() || end < 0) {
    problem = {"cannot read " + name, load_unreadable_message};
    return std::nullopt;
  }
  const auto file_size = static_cast<std::uint64_t>(end);
  if (file_size > MAX_SAVE_FILE_SIZE) {
    problem.detail = name + " is " + std::to_string(file_size) +
                     " bytes, more than a save can be";
    return std::nullopt;
  }

  SaveHeader hdr;
  if (file_size < sizeof(hdr) ||
      !file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
    problem.detail = name + " is too short to be a save";
    return std::nullopt;
  }
  if (std::memcmp(hdr.magic, "PLSAVE", 6) != 0) {
    problem.detail = name + " is not a save file";
    return std::nullopt;
  }
  const uint16_t header_version = endian::from_le16(hdr.version);
  if (header_version > SAVE_HEADER_VERSION) {
    problem = {name + " has header version " + std::to_string(header_version) +
                   "; this build reads up to " +
                   std::to_string(SAVE_HEADER_VERSION),
               load_newer_message};
    return std::nullopt;
  }
  if (header_version == 0) {
    problem.detail = name + " has header version 0";
    return std::nullopt;
  }

  const uint64_t json_size = endian::from_le64(hdr.size);
  if (json_size == 0 || json_size > MAX_SAVE_JSON_SIZE) {
    problem.detail =
        name + " claims " + std::to_string(json_size) + " bytes of game data";
    return std::nullopt;
  }

  std::uint64_t payload_offset = sizeof(SaveHeader);
  if (header_version >= 2) {
    uint32_t meta_len = 0;
    if (!file.read(reinterpret_cast<char*>(&meta_len), sizeof(meta_len))) {
      problem.detail = name + " ends inside its header";
      return std::nullopt;
    }
    meta_len = endian::from_le32(meta_len);
    if (meta_len > MAX_METADATA_SIZE) {
      problem.detail =
          name + " has a " + std::to_string(meta_len) + " byte metadata block";
      return std::nullopt;
    }
    payload_offset += sizeof(meta_len) + meta_len;
  }
  if (payload_offset >= file_size) {
    problem.detail = name + " has no game data after its header";
    return std::nullopt;
  }

  std::vector<unsigned char> compressed(
      static_cast<std::size_t>(file_size - payload_offset));
  file.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(compressed.data()),
                 static_cast<std::streamsize>(compressed.size()))) {
    problem = {"cannot read the game data in " + name, load_unreadable_message};
    return std::nullopt;
  }

  std::string json_str(static_cast<size_t>(json_size), '\0');
  auto dest_len = static_cast<uLongf>(json_size);
  int zret =
      uncompress(reinterpret_cast<Bytef*>(json_str.data()), &dest_len,
                 compressed.data(), static_cast<uLong>(compressed.size()));
  if (zret != Z_OK) {
    problem.detail = name + ": the game data does not inflate (zlib error " +
                     std::to_string(zret) + ")";
    return std::nullopt;
  }
  if (dest_len != static_cast<uLongf>(json_size)) {
    problem.detail = name + ": the game data inflates to " +
                     std::to_string(dest_len) + " bytes, not the " +
                     std::to_string(json_size) + " its header claims";
    return std::nullopt;
  }
  std::vector<unsigned char>().swap(compressed);

  try {
    return parse_bounded(json_str, MAX_SAVE_JSON_DEPTH, MAX_SAVE_JSON_VALUES);
  }
  catch (const std::exception& e) {
    problem.detail = name + ": the game data is not valid JSON: " + e.what();
    return std::nullopt;
  }
}

// Read the uncompressed metadata block of a version >= 2 save.
std::optional<json> read_metadata_block(const fs::path& full_path) {
  std::ifstream file(full_path, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }

  SaveHeader hdr;
  if (!file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
    return std::nullopt;
  }
  if (std::memcmp(hdr.magic, "PLSAVE", 6) != 0 ||
      endian::from_le16(hdr.version) < 2 ||
      endian::from_le16(hdr.version) > SAVE_HEADER_VERSION) {
    return std::nullopt;
  }

  uint32_t meta_len = 0;
  if (!file.read(reinterpret_cast<char*>(&meta_len), sizeof(meta_len))) {
    return std::nullopt;
  }
  meta_len = endian::from_le32(meta_len);
  if (meta_len == 0 || meta_len > MAX_METADATA_SIZE) {
    return std::nullopt;
  }

  std::string meta_str(meta_len, '\0');
  if (!file.read(meta_str.data(), meta_len)) {
    return std::nullopt;
  }
  return parse_bounded(meta_str, MAX_METADATA_DEPTH, MAX_METADATA_VALUES);
}

// Version 1 numbered a premise for rent before one for sale, so the two swap.
void migrate_premise_status(json& j) {
  if (!j.contains("cities")) {
    return;
  }
  for (json& city : j["cities"]) {
    if (!city.contains("properties")) {
      continue;
    }
    for (json& property : city["properties"]) {
      if (!property.contains("status")) {
        continue;
      }
      const int status = property["status"].get<int>();
      if (status == 1) {
        property["status"] = 2;
      }
      else if (status == 2) {
        property["status"] = 1;
      }
    }
  }
}

// Bring an older save up to the current format. The caller has checked the
// version is one this build reads.
void migrate_save(json& j, int version) {
  if (version < 2) {
    migrate_premise_status(j);
  }
  j["version"] = SAVE_FORMAT_VERSION;
}

// The save format version a document gives, if it gives one.
std::optional<int> document_version(const json& j) {
  if (!j.is_object()) {
    return std::nullopt;
  }
  const auto it = j.find("version");
  if (it == j.end() || !it->is_number_integer()) {
    return std::nullopt;
  }
  const auto version = it->get<std::int64_t>();
  if (version < std::numeric_limits<int>::min() ||
      version > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(version);
}

// Structural checks, run on the migrated document before any of it reaches a
// GameState. They cover what the loader uses as an index or a reference, and
// the shapes it assumes; a missing or mistyped field further in throws while
// building and is caught there.

// A save whose structure is wrong; what() says where.
class InvalidSave : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void reject(const std::string& what) { throw InvalidSave(what); }

constexpr std::int64_t int_min = std::numeric_limits<int>::min();
constexpr std::int64_t int_max = std::numeric_limits<int>::max();

std::string item(std::string_view list, std::size_t index) {
  return std::string(list) + "[" + std::to_string(index) + "]";
}

std::string member_name(std::string_view where, std::string_view key) {
  if (where.empty()) {
    return std::string(key);
  }
  return std::string(where) + "." + std::string(key);
}

const json* find_member(const json& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

const json& require_member(const json& object, std::string_view key,
                           std::string_view where = {}) {
  const json* value = find_member(object, key);
  if (value == nullptr) {
    reject("missing " + member_name(where, key));
  }
  return *value;
}

std::int64_t check_integer(const json& value, std::int64_t lowest,
                           std::int64_t highest, const std::string& what) {
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto u = value.get<std::uint64_t>();
    if (u >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      reject(what + " is out of range");
    }
    number = static_cast<std::int64_t>(u);
  }
  else if (value.is_number_integer()) {
    number = value.get<std::int64_t>();
  }
  else {
    reject(what + " is not a whole number");
  }
  if (number < lowest || number > highest) {
    reject(what + " is out of range");
  }
  return number;
}

int check_int(const json& value, std::int64_t lowest, std::int64_t highest,
              const std::string& what) {
  return static_cast<int>(check_integer(value, lowest, highest, what));
}

void check_bool(const json& value, const std::string& what) {
  if (!value.is_boolean()) {
    reject(what + " is not true or false");
  }
}

void check_string(const json& value, const std::string& what) {
  if (!value.is_string()) {
    reject(what + " is not text");
  }
}

// The checks that hand back their value take its name as a string_view, so
// that no temporary is bound to a reference while the result is in use.
const json& check_object(const json& value, std::string_view what) {
  if (!value.is_object()) {
    reject(std::string(what) + " is not an object");
  }
  return value;
}

const json&
check_array(const json& value, std::string_view what,
            std::size_t max_size = std::numeric_limits<std::size_t>::max()) {
  if (!value.is_array()) {
    reject(std::string(what) + " is not a list");
  }
  if (value.size() > max_size) {
    reject(std::string(what) + " has " + std::to_string(value.size()) +
           " entries, more than " + std::to_string(max_size));
  }
  return value;
}

void check_bool_list(const json& value, std::size_t size,
                     const std::string& what) {
  check_array(value, what);
  if (value.size() != size) {
    reject(what + " has " + std::to_string(value.size()) + " entries, not " +
           std::to_string(size));
  }
  for (std::size_t i = 0; i < size; ++i) {
    check_bool(value[i], item(what, i));
  }
}

// A value one of E's enumerators has.
template <typename E>
void check_enum(const json& value, const std::string& what) {
  using U = magic_enum::underlying_type_t<E>;
  const std::int64_t number =
      check_integer(value, std::numeric_limits<U>::min(),
                    std::numeric_limits<U>::max(), what);
  if (!magic_enum::enum_contains<E>(static_cast<U>(number))) {
    reject(what + " is not a known value");
  }
}

void check_player_id(const json& value, const std::string& what) {
  check_int(value, 0, NUMBER_OF_PLAYERS - 1, what);
}

void check_city_id(const json& value, const std::string& what) {
  check_int(value, 0, NUMBER_OF_CITIES - 1, what);
}

void check_restaurant_id(const json& value, const std::string& what) {
  check_int(value, 0, int_max, what);
}

// The y, m and d of an entry: a calendar date GameDate can hold.
void check_date(const json& entry, const std::string& where) {
  check_int(require_member(entry, "y", where), calendar_epoch_year,
            last_save_year, member_name(where, "y"));
  const int month = check_int(require_member(entry, "m", where), 1, 12,
                              member_name(where, "m"));
  check_int(require_member(entry, "d", where), 1,
            days_in_month[static_cast<std::size_t>(month - 1)],
            member_name(where, "d"));
}

// A dated milestone: a mission target's or a CV entry.
void check_dated_milestone(const json& entry, const std::string& what) {
  check_object(entry, what);
  check_int(require_member(entry, "id", what), 1,
            core::events::max_milestone_id, member_name(what, "id"));
  check_date(entry, what);
}

// The entries of an optional list, each checked by check_entry.
template <typename Check>
void check_list(const json& j, std::string_view key, Check check_entry) {
  const json* list = find_member(j, key);
  if (list == nullptr) {
    return;
  }
  const std::string what(key);
  check_array(*list, what);
  for (std::size_t i = 0; i < list->size(); ++i) {
    const std::string entry = item(what, i);
    check_entry(check_object((*list)[i], entry), entry);
  }
}

void check_players(const json& j) {
  const json& players =
      check_array(require_member(j, "players"), "players", NUMBER_OF_PLAYERS);
  for (std::size_t i = 0; i < players.size(); ++i) {
    const json& player = players[i];
    if (player.is_null()) {
      continue;
    }
    const std::string what = item("players", i);
    check_object(player, what);
    // The loader puts each player at its place in the list.
    if (check_int(require_member(player, "id", what), 0, NUMBER_OF_PLAYERS - 1,
                  member_name(what, "id")) != static_cast<int>(i)) {
      reject(member_name(what, "id") + " does not match its place");
    }
    check_bool(require_member(player, "human_controlled", what),
               member_name(what, "human_controlled"));
    check_string(require_member(player, "name", what),
                 member_name(what, "name"));
    const std::string character_name = member_name(what, "character");
    const json& character =
        check_object(require_member(player, "character", what), character_name);
    check_int(require_member(character, "id", character_name), 0, 255,
              member_name(character_name, "id"));
  }

  // The game runs as the active player from the first frame.
  const int active = check_int(require_member(j, "active_player"), 0,
                               NUMBER_OF_PLAYERS - 1, "active_player");
  if (static_cast<std::size_t>(active) >= players.size() ||
      players[static_cast<std::size_t>(active)].is_null()) {
    reject("active_player is not one of the players");
  }
}

void check_property(const json& property, const std::string& what) {
  if (const json* owner = find_member(property, "owner_or_tenant");
      owner != nullptr && !owner->is_null()) {
    check_player_id(*owner, member_name(what, "owner_or_tenant"));
  }
  if (const json* restaurant = find_member(property, "restaurant_id");
      restaurant != nullptr && !restaurant->is_null()) {
    check_restaurant_id(*restaurant, member_name(what, "restaurant_id"));
  }
}

void check_cities(const json& j) {
  const json* cities = find_member(j, "cities");
  if (cities == nullptr) {
    return;
  }
  check_array(*cities, "cities", NUMBER_OF_CITIES);
  std::array<bool, NUMBER_OF_CITIES> seen{};
  for (std::size_t i = 0; i < cities->size(); ++i) {
    const std::string what = item("cities", i);
    const json& city = check_object((*cities)[i], what);
    const int id = check_int(require_member(city, "id", what), 0,
                             NUMBER_OF_CITIES - 1, member_name(what, "id"));
    if (seen[static_cast<std::size_t>(id)]) {
      reject(what + " repeats city " + std::to_string(id));
    }
    seen[static_cast<std::size_t>(id)] = true;

    const std::string properties_name = member_name(what, "properties");
    const json& properties =
        check_array(require_member(city, "properties", what), properties_name);
    for (std::size_t p = 0; p < properties.size(); ++p) {
      const std::string property = item(properties_name, p);
      check_property(check_object(properties[p], property), property);
    }

    // Who leads the pizza charts and owns each block of the control map.
    if (const json* charts = find_member(city, "pizza_charts")) {
      if (const json* entries = find_member(*charts, "entries")) {
        const std::string entries_name =
            member_name(what, "pizza_charts.entries");
        check_array(*entries, entries_name);
        for (std::size_t e = 0; e < entries->size(); ++e) {
          const std::string entry = item(entries_name, e);
          if (const json* player = find_member(
                  check_object((*entries)[e], entry), "player_id")) {
            check_player_id(*player, member_name(entry, "player_id"));
          }
        }
      }
    }
    if (const json* control = find_member(city, "control_map")) {
      if (const json* owners = find_member(*control, "cell_owners")) {
        const std::string owners_name =
            member_name(what, "control_map.cell_owners");
        check_array(*owners, owners_name);
        for (std::size_t c = 0; c < owners->size(); ++c) {
          check_int((*owners)[c], -1, NUMBER_OF_PLAYERS - 1,
                    item(owners_name, c));
        }
      }
    }
  }
}

void check_restaurants(const json& j) {
  std::unordered_set<int> ids;
  check_list(j, "restaurants",
             [&ids](const json& restaurant, const std::string& what) {
               const int id = check_int(require_member(restaurant, "id", what),
                                        0, int_max, member_name(what, "id"));
               if (!ids.insert(id).second) {
                 reject(what + " repeats restaurant " + std::to_string(id));
               }
               check_city_id(
                   require_member(restaurant, "property_city_id", what),
                   member_name(what, "property_city_id"));
               check_int(require_member(restaurant, "property_index", what),
                         int_min, int_max, member_name(what, "property_index"));
             });
  if (const json* active = find_member(j, "active_restaurant");
      active != nullptr && !active->is_null()) {
    check_restaurant_id(*active, "active_restaurant");
  }
}

void check_turn_state(const json& j) {
  // Flags the turn keeps per city, and per city and player.
  if (const json* flags = find_member(j, "city_initialised")) {
    check_bool_list(*flags, NUMBER_OF_CITIES, "city_initialised");
  }
  if (const json* flags = find_member(j, "city_start_of_day_ran")) {
    check_bool_list(*flags, NUMBER_OF_CITIES, "city_start_of_day_ran");
  }
  if (const json* flags = find_member(j, "city_ai_action_taken")) {
    check_bool_list(*flags, NUMBER_OF_CITIES, "city_ai_action_taken");
  }
  if (const json* flags = find_member(j, "restaurant_day_ran")) {
    check_array(*flags, "restaurant_day_ran");
    if (flags->size() != NUMBER_OF_CITIES) {
      reject("restaurant_day_ran has " + std::to_string(flags->size()) +
             " entries, not " + std::to_string(NUMBER_OF_CITIES));
    }
    for (std::size_t i = 0; i < flags->size(); ++i) {
      check_bool_list((*flags)[i], NUMBER_OF_PLAYERS,
                      item("restaurant_day_ran", i));
    }
  }

  check_list(j, "deferred_staff_events",
             [](const json& event, const std::string& what) {
               check_restaurant_id(require_member(event, "restaurant_id", what),
                                   member_name(what, "restaurant_id"));
               check_enum<core::restaurant::StaffEventData::Type>(
                   require_member(event, "type", what),
                   member_name(what, "type"));
               check_enum<core::character::JobRole>(
                   require_member(event, "job_role", what),
                   member_name(what, "job_role"));
               check_int(require_member(event, "slot_index", what), int_min,
                         int_max, member_name(what, "slot_index"));
             });

  check_list(
      j, "deferred_calls", [](const json& call, const std::string& what) {
        using Kind = TurnOrchestrator::DeferredCall::Kind;
        check_enum<Kind>(require_member(call, "kind", what),
                         member_name(what, "kind"));
        if (const json* player = find_member(call, "player_id")) {
          check_player_id(*player, member_name(what, "player_id"));
        }
        switch (static_cast<Kind>(call.at("kind").get<int>())) {
        case Kind::MayorInspection:
          check_restaurant_id(require_member(call, "restaurant_id", what),
                              member_name(what, "restaurant_id"));
          check_bool(require_member(call, "is_first", what),
                     member_name(what, "is_first"));
          if (const json* block = find_member(call, "display_block")) {
            check_int(*block, int_min, int_max,
                      member_name(what, "display_block"));
          }
          break;
        case Kind::MayorAward:
          check_enum<MayorAwardType>(require_member(call, "award_type", what),
                                     member_name(what, "award_type"));
          if (const json* announces = find_member(call, "announces_winner")) {
            check_bool(*announces, member_name(what, "announces_winner"));
          }
          // -1 is an award nobody qualified for.
          if (const json* winner = find_member(call, "winner_player_id");
              winner != nullptr && !winner->is_null()) {
            check_int(*winner, -1, NUMBER_OF_PLAYERS - 1,
                      member_name(what, "winner_player_id"));
          }
          break;
        case Kind::DepotRaid:
          break;
        }
      });

  check_list(j, "deferred_racket_offers",
             [](const json& offer, const std::string& what) {
               check_player_id(require_member(offer, "player_id", what),
                               member_name(what, "player_id"));
               check_restaurant_id(require_member(offer, "restaurant_id", what),
                                   member_name(what, "restaurant_id"));
               check_bool(require_member(offer, "first_contact", what),
                          member_name(what, "first_contact"));
             });

  check_list(j, "deferred_manager_alerts",
             [](const json& alert, const std::string& what) {
               check_restaurant_id(require_member(alert, "restaurant_id", what),
                                   member_name(what, "restaurant_id"));
               check_string(require_member(alert, "message", what),
                            member_name(what, "message"));
               check_int(require_member(alert, "display_block", what), int_min,
                         int_max, member_name(what, "display_block"));
             });
}

void check_mission(const json& j) {
  const json* mission = find_member(j, "mission_target");
  if (mission == nullptr) {
    return;
  }
  check_object(*mission, "mission_target");
  if (const json* source = find_member(*mission, "source_name")) {
    check_string(*source, "mission_target.source_name");
  }
  // MissionTarget::final_event() reads the last milestone, so there has to be
  // one, as a mission file always has.
  const json& milestones =
      check_array(require_member(*mission, "milestones", "mission_target"),
                  "mission_target.milestones");
  if (milestones.empty()) {
    reject("mission_target has no milestones");
  }
  for (std::size_t i = 0; i < milestones.size(); ++i) {
    check_dated_milestone(milestones[i], item("mission_target.milestones", i));
  }
  if (const json* rules = find_member(j, "mission_rules")) {
    check_object(*rules, "mission_rules");
  }
}

void check_career_logs(const json& j) {
  // A game where nobody has a CV entry yet saves this as null.
  const json* shown = find_member(j, "shown_messages");
  if (shown == nullptr || shown->is_null()) {
    return;
  }
  check_object(*shown, "shown_messages");
  for (const auto& [key, entries] : shown->items()) {
    const std::string what = member_name("shown_messages", key);
    int player = -1;
    const char* first = key.data();
    const char* last = first + key.size();
    const auto [rest, ec] = std::from_chars(first, last, player);
    if (ec != std::errc{} || rest != last || player < 0 ||
        player >= NUMBER_OF_PLAYERS) {
      reject(what + " is not a player number");
    }
    check_array(entries, what);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const std::string entry = item(what, i);
      check_dated_milestone(entries[i], entry);
      if (const json* was_shown = find_member(entries[i], "shown")) {
        check_bool(*was_shown, member_name(entry, "shown"));
      }
    }
  }
}

void check_scalars(const json& j) {
  if (const json* blocks =
          find_member(require_member(j, "time"), "blocks_since_start")) {
    check_int(*blocks, 0, int_max, "time.blocks_since_start");
  }
  if (const json* city = find_member(j, "active_city")) {
    // Out of range is allowed: the loader falls back to a city it has.
    check_int(*city, int_min, int_max, "active_city");
  }
  if (const json* mode = find_member(j, "game_mode")) {
    check_enum<GameMode>(*mode, "game_mode");
  }
  if (const json* level = find_member(j, "difficulty_level")) {
    check_enum<init::DifficultyLevel>(*level, "difficulty_level");
  }
  if (const json* district = find_member(j, "district_under_investigation")) {
    check_int(*district, int_min, int_max, "district_under_investigation");
  }
  if (const json* category = find_member(j, "price_update_category")) {
    check_int(*category, 0, static_cast<int>(ingredient_category_count) - 1,
              "price_update_category");
  }
  if (const json* phase = find_member(j, "interruption_phase")) {
    // Out of range is allowed: the loader restarts the round-robin.
    check_int(*phase, int_min, int_max, "interruption_phase");
  }
  if (const json* second = find_member(j, "second_in_block")) {
    check_int(*second, 0, second_counter_wrap - 1, "second_in_block");
  }
  if (const json* tick = find_member(j, "pit_tick_in_second")) {
    check_int(*tick, 0, pit_ticks_per_second - 1, "pit_tick_in_second");
  }
  if (const json* charge = find_member(j, "pending_screen_charge_blocks")) {
    check_int(*charge, int_min, int_max, "pending_screen_charge_blocks");
  }
  if (const json* rng = find_member(j, "rng_state")) {
    check_integer(*rng, 0, std::numeric_limits<std::uint32_t>::max(),
                  "rng_state");
  }
  if (const json* cities = find_member(j, "player_cities")) {
    check_array(*cities, "player_cities", NUMBER_OF_PLAYERS);
    for (std::size_t i = 0; i < cities->size(); ++i) {
      if (!(*cities)[i].is_null()) {
        check_city_id((*cities)[i], item("player_cities", i));
      }
    }
  }
}

// Throws InvalidSave if the document is not a save the loader can build a
// game from.
void validate_save_document(const json& j) {
  if (!j.is_object()) {
    reject("the save is not a JSON object");
  }
  check_object(require_member(j, "time"), "time");
  check_players(j);
  check_cities(j);
  check_restaurants(j);
  check_turn_state(j);
  check_mission(j);
  check_career_logs(j);
  check_scalars(j);
}

// Reads, upgrades and checks a save. Returns the document ready for
// json_to_state, or nullopt with the problem filled in.
std::optional<json> read_save_document(const fs::path& path,
                                       LoadProblem& problem) {
  try {
    std::optional<json> document = decode_save_file(path, problem);
    if (!document) {
      return std::nullopt;
    }
    const std::optional<int> version = document_version(*document);
    if (!version) {
      problem.detail = display(path) + " has no save format version";
      return std::nullopt;
    }
    if (*version > SAVE_FORMAT_VERSION) {
      problem = {display(path) + " is save format " + std::to_string(*version) +
                     "; this build reads up to " +
                     std::to_string(SAVE_FORMAT_VERSION),
                 load_newer_message};
      return std::nullopt;
    }
    if (*version < OLDEST_LOADABLE_SAVE_VERSION) {
      problem.detail = display(path) + " is save format " +
                       std::to_string(*version) + ", which no build reads";
      return std::nullopt;
    }
    migrate_save(*document, *version);
    validate_save_document(*document);
    return document;
  }
  catch (const std::exception& e) {
    problem = {display(path) + ": " + e.what(), load_damaged_message};
    return std::nullopt;
  }
}

} // namespace

std::string SaveGameManager::get_full_path(const std::string& filename) {
  // An absolute filename replaces the folder.
  return (fs::path(platform_paths::saves_dir()) / filename).string();
}

bool SaveGameManager::save(const GameState& state, const GameContext& context,
                           const std::string& filename, std::string* error) {
  Logger::info("Saving game to {}", filename);
  const fs::path path = get_full_path(filename);

  std::string bytes;
  try {
    bytes = encode_save(state_to_json(state, context));
  }
  catch (const std::exception& e) {
    Logger::warn("Save failed: {}: {}", display(path), e.what());
    set_message(error, save_failed_message);
    return false;
  }

  if (const std::error_code ec = create_parent_folder(path)) {
    Logger::warn("Save failed: cannot create the folder for {}: {}",
                 display(path), ec.message());
    set_message(error, save_failure_message(ec));
    return false;
  }

  const util::AtomicWriteResult written =
      util::write_file_atomically(path, bytes);
  if (!written) {
    Logger::warn("Save failed: {}", written.error);
    set_message(error, save_failure_message(written.code));
    return false;
  }

  Logger::info("Save completed successfully");
  return true;
}

namespace {

std::string autosave_slot_filename(int slot) {
  return std::string(autosave_prefix) + std::to_string(slot) + ".plsave";
}

std::string next_autosave_filename(int slot_count) {
  fs::path saves_dir = platform_paths::saves_dir();

  // Fill slots in order before overwriting any
  for (int i = 1; i <= slot_count; ++i) {
    std::error_code ec;
    if (!fs::exists(saves_dir / autosave_slot_filename(i), ec)) {
      return autosave_slot_filename(i);
    }
  }

  // All slots filled; overwrite the one with the oldest mtime
  int oldest_slot = 1;
  fs::file_time_type oldest_time = fs::file_time_type::max();
  for (int i = 1; i <= slot_count; ++i) {
    std::error_code ec;
    auto mtime = fs::last_write_time(saves_dir / autosave_slot_filename(i), ec);
    if (!ec && mtime < oldest_time) {
      oldest_time = mtime;
      oldest_slot = i;
    }
  }
  return autosave_slot_filename(oldest_slot);
}

} // namespace

bool SaveGameManager::is_autosave_filename(const std::string& filename) {
  if (filename.size() < autosave_prefix.size()) {
    return false;
  }
  return std::equal(autosave_prefix.begin(), autosave_prefix.end(),
                    filename.begin(), [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

bool SaveGameManager::autosave(const GameState& state,
                               const GameContext& context, std::string* error) {
  const int slot_count = std::clamp(state.options().autosave_count,
                                    GameOptions::AUTOSAVE_COUNT_MIN,
                                    GameOptions::AUTOSAVE_COUNT_MAX);
  std::string filename = next_autosave_filename(slot_count);
  Logger::info("Autosaving to {}", filename);
  bool ok = save(state, context, filename, error);
  if (!ok) {
    Logger::warn("Autosave failed");
  }
  return ok;
}

std::optional<SaveMetadata>
SaveGameManager::read_save_metadata(const std::string& filename) {
  try {
    const fs::path path = get_full_path(filename);
    std::optional<json> maybe_meta = read_metadata_block(path);
    if (!maybe_meta) {
      // Version 1 save: the metadata lives only in the full document.
      LoadProblem problem;
      std::optional<json> document = decode_save_file(path, problem);
      if (!document) {
        Logger::warn("Cannot read save details: {}", problem.detail);
        return std::nullopt;
      }
      maybe_meta = metadata_from_save_json(*document);
    }
    const json& m = *maybe_meta;

    SaveMetadata meta;

    meta.display_name = filename;
    if (meta.display_name.ends_with(".plsave")) {
      meta.display_name =
          meta.display_name.substr(0, meta.display_name.size() - 7);
    }

    meta.saved_at = m.value("saved_at", "Unknown");
    meta.game_date = m.value("game_date", "");

    if (m.contains("player_names")) {
      const auto& names = m.at("player_names");
      for (size_t i = 0;
           i < names.size() && i < static_cast<size_t>(NUMBER_OF_PLAYERS);
           ++i) {
        meta.player_names[i] = names.at(i).get<std::string>();
      }
    }

    meta.active_player = m.value("active_player", -1);
    meta.active_city = m.value("active_city", -1);

    return meta;
  }
  catch (const std::exception& e) {
    Logger::warn("Cannot read save details from {}: {}", filename, e.what());
    return std::nullopt;
  }
}

bool SaveGameManager::load(GameState& state, GameContext& context,
                           const std::string& filename, std::string* error) {
  Logger::info("Loading game from {}", filename);
  const fs::path path = get_full_path(filename);

  LoadProblem problem;
  std::optional<json> document = read_save_document(path, problem);
  if (!document) {
    Logger::warn("Save rejected: {}", problem.detail);
    set_message(error, problem.message);
    return false;
  }

  // Build the game from the save in a scratch state first. Damage the checks
  // above can't see, such as a field missing deep inside a restaurant, shows
  // up here while the running game is still untouched. Building draws from the
  // shared random generator, so that is put back afterwards.
  Logger::info("Checking the save by building it apart from the running game");
  const uint32_t random_state = engine::random::get_state();
  bool built = false;
  std::string build_error;
  try {
    auto scratch = std::make_unique<GameState>();
    scratch->options() = state.options();
    json_to_state(*document, *scratch, context);
    built = true;
  }
  catch (const std::exception& e) {
    build_error = e.what();
  }
  catch (...) {
    build_error = "unknown error";
  }
  engine::random::set_state(random_state);
  if (!built) {
    Logger::warn("Save rejected: {}: {}", display(path), build_error);
    set_message(error, load_damaged_message);
    return false;
  }

  // Every check passed, so load for real. It is the same build from the same
  // document and the same random state, so it goes as the scratch one did.
  Logger::info("Save checked; loading it into the game");
  try {
    json_to_state(*document, state, context);
    state.finalize_initialization(context);

    // Land in the city view for the loaded city
    state.ui().pop_to_root();
    state.ui().replace_top<CityViewState>(state.active_city_id());
  }
  catch (const std::exception& e) {
    Logger::warn("Load of {} failed after its checks passed: {}", display(path),
                 e.what());
    set_message(error, load_failed_message);
    return false;
  }

  Logger::info("Load completed successfully");
  return true;
}

bool SaveGameManager::is_valid_save(const std::string& filename) {
  LoadProblem problem;
  if (read_save_document(get_full_path(filename), problem)) {
    return true;
  }
  Logger::warn("Not a valid save: {}", problem.detail);
  return false;
}

int SaveGameManager::get_save_version(const std::string& filename) {
  try {
    LoadProblem problem;
    const std::optional<json> document =
        decode_save_file(get_full_path(filename), problem);
    if (!document) {
      return -1;
    }
    return document_version(*document).value_or(-1);
  }
  catch (const std::exception& e) {
    Logger::warn("Cannot read the version of {}: {}", filename, e.what());
    return -1;
  }
}

namespace {

json time_to_json(const GameTime& time) {
  return {
      {"blocks_since_start", time.time_blocks()},
      {"display", time.to_string()} // Human-readable for debugging
  };
}

GameTime json_to_time(const json& j) {
  GameTime time;
  if (j.contains("blocks_since_start")) {
    time.time_blocks(j.at("blocks_since_start").get<int>());
  }
  return time;
}

json rules_to_json(const GameRules& rules) {
  return {
      {"fix_legacy_bugs", rules.fix_legacy_bugs},
      {"mafia_shakedowns_target_all_restaurants",
       rules.mafia_shakedowns_target_all_restaurants},
      {"demand_fulfillment_popularity", rules.demand_fulfillment_popularity},
      {"win_condition", to_config_value(rules.win_condition)},
      {"pizza_judging", to_config_value(rules.pizza_judging)},
      {"cook_book", rules.cook_book},
      {"police", rules.police},
      {"suppliers_remember_missed_payments",
       rules.suppliers_remember_missed_payments},
  };
}

// Missing keys use the values in rules.
GameRules json_to_rules(const json& j, GameRules rules) {
  rules.fix_legacy_bugs = j.value("fix_legacy_bugs", rules.fix_legacy_bugs);
  rules.mafia_shakedowns_target_all_restaurants =
      j.value("mafia_shakedowns_target_all_restaurants",
              rules.mafia_shakedowns_target_all_restaurants);
  rules.demand_fulfillment_popularity = j.value(
      "demand_fulfillment_popularity", rules.demand_fulfillment_popularity);
  rules.suppliers_remember_missed_payments =
      j.value("suppliers_remember_missed_payments",
              rules.suppliers_remember_missed_payments);
  if (auto parsed = win_condition_from_config_value(
          j.value("win_condition", std::string{}))) {
    rules.win_condition = *parsed;
  }
  if (auto parsed = pizza_judging_from_config_value(
          j.value("pizza_judging", std::string{}))) {
    rules.pizza_judging = *parsed;
  }
  rules.police = j.value("police", rules.police);
  rules.cook_book = j.value("cook_book", rules.cook_book);
  return rules;
}

} // namespace

json SaveGameManager::state_to_json(const GameState& state,
                                    const GameContext& /*context*/) {
  json j;

  // Save format version
  j["version"] = SAVE_FORMAT_VERSION;

  // Save RNG state so a loaded game resumes with the same random sequence
  j["rng_state"] = engine::random::get_state();

  std::time_t now = std::time(nullptr);
  j["saved_at"] = std::string(util::trim(std::ctime(&now)));

  // Save game time
  j["time"] = time_to_json(state.time());

  // Save active IDs
  j["active_player"] = state.active_player()->id().value;
  j["active_city"] = state.active_city_id().value;
  j["city_initialised"] = state.city_initialised_flags();
  j["city_start_of_day_ran"] = state.turn().city_start_of_day_flags();
  j["restaurant_day_ran"] = state.turn().restaurant_day_flags();
  j["city_ai_action_taken"] = state.turn().ai_action_flags();

  if (state.active_restaurant()) {
    j["active_restaurant"] = state.active_restaurant()->id().value;
  }
  else {
    j["active_restaurant"] = nullptr;
  }

  // Save difficulty
  j["difficulty_level"] = state.difficulty_level();
  j["rules"] = rules_to_json(state.rules());
  j["starting_rules"] = rules_to_json(state.starting_rules());
  j["rules_changed"] = state.rules_changed();
  if (auto district = state.district_under_investigation()) {
    j["district_under_investigation"] = *district;
  }
  j["game_mode"] = static_cast<int>(state.game_mode());

  // Save the mission target CV (full dates, so loaded games never depend
  // on the .mis file or its year encoding again)
  if (state.mission_target().has_value()) {
    const auto& target = *state.mission_target();
    json mission_array = json::array();
    for (const auto& milestone : target.milestones) {
      mission_array.push_back({{"id", static_cast<int>(milestone.message_id)},
                               {"y", milestone.date.year()},
                               {"m", milestone.date.month()},
                               {"d", milestone.date.day()}});
    }
    if (target.rules) {
      j["mission_rules"] = rules_to_json(*target.rules);
      j["mission_rules_changed"] = target.rules_changed;
    }
    j["mission_target"] = {{"source_name", target.source_name},
                           {"milestones", mission_array}};
  }

  // Save customer taste profiles
  {
    json taste_array = json::array();
    for (auto demo : ALL_CONSUMER_TYPES) {
      const auto& tp = state.customer_taste_profiles()[demo];
      taste_array.push_back({{"fat", tp.fat},
                             {"sweet", tp.sweet},
                             {"salty", tp.salty},
                             {"hot", tp.hot},
                             {"spicy", tp.spicy}});
    }
    j["customer_taste_profiles"] = std::move(taste_array);
  }

  // Save the underworld price walk for every city
  {
    json market_array = json::array();
    const auto& market = state.weapon_market();
    for (int i = 0; i < NUMBER_OF_CITIES; ++i) {
      core::CityId city_id{i};
      for (int column = 0; column < WeaponMarket::columns_per_city; ++column) {
        market_array.push_back({{"modifier", market.modifier(city_id, column)},
                                {"rising", market.rising(city_id, column)}});
      }
    }
    j["weapon_market"] = std::move(market_array);
  }

  // Save players
  json players_array = json::array();
  auto players = state.players();
  for (size_t i = 0; i < players.size(); ++i) {
    if (players[i] != nullptr) {
      players_array.push_back(player_to_json(*players[i]));
    }
    else {
      players_array.push_back(nullptr);
    }
  }
  j["players"] = std::move(players_array);

  // Save visited cities only
  json cities_array = json::array();
  auto visited = state.initialized_cities();
  for (City* city : visited) {
    cities_array.push_back(city_to_json(*city));
  }
  j["cities"] = std::move(cities_array);

  // Save restaurants
  json restaurants_array = json::array();
  // We need to access the restaurants through GameState methods
  // For now, save all restaurants we can find through players
  for (size_t pid = 0; pid < players.size(); ++pid) {
    if (players[pid] != nullptr) {
      auto player_restaurants =
          state.restaurants_for(core::PlayerId{static_cast<int>(pid)});
      for (auto* restaurant : player_restaurants) {
        if (restaurant) {
          restaurants_array.push_back(restaurant_to_json(*restaurant));
        }
      }
    }
  }
  j["restaurants"] = std::move(restaurants_array);

  // Save deferred staff events (queued at EOD, delivered at 10am next day)
  {
    json staff_array = json::array();
    for (const auto& ev : state.turn().deferred_staff_events()) {
      staff_array.push_back({{"restaurant_id", ev.restaurant_id.value},
                             {"type", static_cast<int>(ev.event.type)},
                             {"job_role", static_cast<int>(ev.event.job_role)},
                             {"slot_index", ev.event.slot_index}});
    }
    j["deferred_staff_events"] = std::move(staff_array);
  }

  // The mayor's and police's deferred calls, in the order they go out
  {
    using Kind = TurnOrchestrator::DeferredCall::Kind;
    json queued_array = json::array();
    for (const auto& ev : state.turn().deferred_calls()) {
      json entry = {{"kind", static_cast<int>(ev.kind)},
                    {"player_id", ev.player_id.value}};
      switch (ev.kind) {
      case Kind::MayorInspection:
        entry["restaurant_id"] = ev.restaurant_id.value;
        entry["is_first"] = ev.is_first;
        entry["display_block"] = ev.display_block;
        break;
      case Kind::MayorAward:
        entry["award_type"] = static_cast<int>(ev.award.type);
        entry["announces_winner"] = ev.award.announces_winner;
        if (ev.award.winner_player_id.has_value()) {
          entry["winner_player_id"] = *ev.award.winner_player_id;
        }
        else {
          entry["winner_player_id"] = nullptr;
        }
        break;
      case Kind::DepotRaid:
        break;
      }
      queued_array.push_back(std::move(entry));
    }
    j["deferred_calls"] = std::move(queued_array);
  }

  // Save deferred protection racket offers (rolled at EOD, delivered on the
  // owning player's next turn)
  {
    json racket_array = json::array();
    for (const auto& offer : state.turn().deferred_racket_offers()) {
      racket_array.push_back({{"player_id", offer.player_id.value},
                              {"restaurant_id", offer.restaurant_id.value},
                              {"first_contact", offer.first_contact}});
    }
    j["deferred_racket_offers"] = std::move(racket_array);
  }

  {
    json alert_array = json::array();
    for (const auto& alert : state.turn().deferred_manager_alerts()) {
      alert_array.push_back({{"restaurant_id", alert.restaurant_id.value},
                             {"message", alert.message},
                             {"display_block", alert.display_block}});
    }
    j["deferred_manager_alerts"] = std::move(alert_array);
  }

  j["floor_editing_bonus_shown"] = state.turn().floor_editing_bonus_shown();
  j["competition_count"] = state.competitions().competition_count();
  j["price_update_category"] = state.price_update_category();
  j["interruption_phase"] = static_cast<int>(state.interruption_phase());
  j["second_in_block"] = state.second_in_block();
  j["pit_tick_in_second"] = state.pit_tick_in_second();
  j["pending_screen_charge_blocks"] = state.pending_screen_charge();

  json player_cities = json::array();
  for (const auto& city_id : state.player_cities()) {
    player_cities.push_back(city_id ? json(city_id->value) : json(nullptr));
  }
  j["player_cities"] = std::move(player_cities);

  // Save shown message history with dates (for CV display and deduplication)
  json shown_msgs;
  for (size_t pid = 0; pid < players.size(); ++pid) {
    if (players[pid] != nullptr) {
      auto player_id = core::PlayerId{static_cast<int>(pid)};
      auto entries = state.career_log().get_cv_entries_for(player_id);
      if (!entries.empty()) {
        json entry_array = json::array();
        for (const auto& entry : entries) {
          entry_array.push_back({{"id", static_cast<int>(entry.message_id)},
                                 {"y", entry.date.year()},
                                 {"m", entry.date.month()},
                                 {"d", entry.date.day()},
                                 {"shown", entry.shown}});
        }
        shown_msgs[std::to_string(pid)] = std::move(entry_array);
      }
    }
  }
  j["shown_messages"] = std::move(shown_msgs);

  return j;
}

void SaveGameManager::json_to_state(const json& j, GameState& state,
                                    GameContext& context) {
  // Clear all existing restaurants and reset property states before loading
  state.reset();

  // Load game time
  if (j.contains("time")) {
    GameTime loaded_time = json_to_time(j.at("time"));
    state.set_time(loaded_time);
  }

  // Load active player ID
  if (j.contains("active_player")) {
    core::PlayerId pid{j.at("active_player").get<int>()};
    state.active_player(pid);
  }
  if (j.contains("game_mode")) {
    auto mode{j.at("game_mode").get<int>()};
    state.game_mode(static_cast<GameMode>(mode));
  }
  if (j.contains("mission_target")) {
    const auto& mt = j.at("mission_target");
    core::mission::MissionTarget target;
    target.source_name = mt.value("source_name", std::string{});
    for (const auto& milestone : mt.at("milestones")) {
      target.milestones.push_back(
          {save_date(milestone.at("y").get<int>(), milestone.at("m").get<int>(),
                     milestone.at("d").get<int>()),
           static_cast<core::events::MilestoneId>(
               milestone.at("id").get<int>())});
    }
    // MissionTarget::final_event() reads the last milestone.
    if (target.milestones.empty()) {
      throw std::out_of_range("the mission target has no milestones");
    }
    if (j.contains("mission_rules")) {
      target.rules = json_to_rules(j.at("mission_rules"), GameRules{});
      target.rules_changed = j.value("mission_rules_changed", false);
    }
    state.mission_target(std::move(target));
  }
  if (j.contains("difficulty_level")) {
    state.difficulty_level(j.at("difficulty_level").get<int>());
  }
  state.rules() =
      json_to_rules(j.value("rules", json::object()), state.options().rules);
  state.starting_rules(
      json_to_rules(j.value("starting_rules", json::object()), state.rules()));
  state.rules_changed(j.value("rules_changed", false));
  if (j.contains("district_under_investigation")) {
    state.district_under_investigation(
        j.at("district_under_investigation").get<int>());
  }

  // active_city and active_restaurant will be set after
  // cities/restaurants are loaded (to avoid them being overwritten during city
  // creation)

  // Load customer taste profiles (before players, since pizza loading uses
  // them)
  if (j.contains("customer_taste_profiles")) {
    const auto& taste_array = j.at("customer_taste_profiles");
    for (size_t i = 0;
         i < taste_array.size() && i < static_cast<size_t>(ConsumerType::Count);
         ++i) {
      auto demo = static_cast<ConsumerType>(i);
      auto& tp = state.customer_taste_profiles()[demo];
      const auto& profile = taste_array.at(i);
      tp.fat = profile.at("fat").get<int>();
      tp.sweet = profile.at("sweet").get<int>();
      tp.salty = profile.at("salty").get<int>();
      tp.hot = profile.at("hot").get<int>();
      tp.spicy = profile.at("spicy").get<int>();
    }
  }

  if (j.contains("weapon_market")) {
    const auto& market_array = j.at("weapon_market");
    auto& market = state.weapon_market();
    constexpr size_t slot_count =
        static_cast<size_t>(NUMBER_OF_CITIES) * WeaponMarket::columns_per_city;
    for (size_t i = 0; i < market_array.size() && i < slot_count; ++i) {
      core::CityId city_id{static_cast<int>(i) /
                           WeaponMarket::columns_per_city};
      int column = static_cast<int>(i) % WeaponMarket::columns_per_city;
      const auto& slot = market_array.at(i);
      market.set_slot(city_id, column, slot.at("modifier").get<int>(),
                      slot.at("rising").get<bool>());
    }
  }

  // Load players
  if (j.contains("players")) {
    const auto& players_json = j.at("players");
    std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
    for (size_t i = 0; i < players_json.size() && i < players.size(); ++i) {
      if (!players_json.at(i).is_null()) {
        players[i] = json_to_player(players_json.at(i), context,
                                    state.customer_taste_profiles());
      }
    }
    state.players(std::move(players));
  }

  // Make sure we set the fact that a city has been initialized before creating
  // a city so it doesn't run init again.
  if (j.contains("city_initialised")) {
    state.city_initialised_flags(
        j.at("city_initialised").get<GameState::CityInitFlags>());
  }

  // Load cities (must be done BEFORE restaurants to restore property state)
  if (j.contains("cities")) {
    const auto& cities_json = j.at("cities");
    Logger::info("Loading {} cities from save file", cities_json.size());
    for (const auto& city_json : cities_json) {
      core::CityId city_id{city_json.at("id").get<int>()};
      if (!is_valid_city_id(city_id)) {
        throw std::out_of_range("the save has a city with id " +
                                std::to_string(city_id.value));
      }
      Logger::info("Loading city {}", city_id.value);

      // Ensure city exists WITHOUT changing active_city
      state.ensure_city_loaded(city_id);
      City* city = state.city(city_id);

      if (city) {
        json_to_city(city_json, *city);
        Logger::info("Restored city {} with {} properties", city_id.value,
                     city_json.at("properties").size());
      }
      else {
        Logger::warn("Failed to create city {}", city_id.value);
      }
    }
  }

  // Load restaurants (after cities so properties are in correct state)
  if (j.contains("restaurants")) {
    for (const auto& rest_json : j.at("restaurants")) {
      // Get the property reference
      core::CityId city_id{rest_json.at("property_city_id").get<int>()};
      int property_index = rest_json.at("property_index").get<int>();

      City* city = is_valid_city_id(city_id) ? state.city(city_id) : nullptr;
      if (!city) {
        Logger::warn("Failed to find city {} for restaurant", city_id.value);
        continue;
      }

      Property* property = city->property_by_index(property_index);
      if (!property) {
        Logger::warn("Failed to find property {} in city {} for restaurant",
                     property_index, city_id.value);
        continue;
      }

      // Reconstruct the restaurant
      auto restaurant = json_to_restaurant(rest_json, *property);

      // Register the restaurant ID with the property
      property->restaurant_id(restaurant->id());

      // Add to game state
      state.add_restaurant(std::move(restaurant));
    }
  }

  // Making a city active runs its start of day unless it already ran, so we
  // have to set these flags first.
  if (j.contains("city_start_of_day_ran")) {
    state.turn().city_start_of_day_flags(
        j.at("city_start_of_day_ran").get<TurnOrchestrator::CityDayFlags>());
  }
  if (j.contains("restaurant_day_ran")) {
    state.turn().restaurant_day_flags(
        j.at("restaurant_day_ran").get<TurnOrchestrator::RestaurantDayFlags>());
  }
  if (j.contains("city_ai_action_taken")) {
    state.turn().ai_action_flags(
        j.at("city_ai_action_taken").get<TurnOrchestrator::CityDayFlags>());
  }
  if (j.contains("active_city")) {
    core::CityId cid{j.at("active_city").get<int>()};
    // If somehow there is no valid active city, just pick one.
    if (!is_valid_city_id(cid)) {
      cid = state.default_city_id();
      Logger::warn("Save has no active city; falling back to city {}",
                   cid.value);
    }
    state.active_city(cid);
    Logger::info("Set active city to {}", cid.value);
  }

  // Set active restaurant after restaurants are loaded
  if (j.contains("active_restaurant") && !j.at("active_restaurant").is_null()) {
    core::RestaurantId rid{j.at("active_restaurant").get<int>()};
    state.active_restaurant(rid);
  }

  // Restore deferred staff events
  if (j.contains("deferred_staff_events")) {
    for (const auto& ev : j.at("deferred_staff_events")) {
      core::restaurant::StaffEventData data;
      data.type = static_cast<core::restaurant::StaffEventData::Type>(
          ev.at("type").get<int>());
      data.job_role =
          static_cast<core::character::JobRole>(ev.at("job_role").get<int>());
      data.slot_index = ev.at("slot_index").get<int>();
      state.turn().restore_deferred_staff_event(
          data, core::RestaurantId{ev.at("restaurant_id").get<int>()});
    }
  }

  // Restore the mayor's and police's deferred calls
  if (j.contains("deferred_calls")) {
    using DeferredCall = TurnOrchestrator::DeferredCall;
    using Kind = DeferredCall::Kind;
    for (const auto& ev : j.at("deferred_calls")) {
      DeferredCall queued;
      queued.kind = static_cast<Kind>(ev.at("kind").get<int>());
      queued.player_id = core::PlayerId{ev.value("player_id", 0)};
      switch (queued.kind) {
      case Kind::MayorInspection:
        queued.restaurant_id =
            core::RestaurantId{ev.at("restaurant_id").get<int>()};
        queued.is_first = ev.at("is_first").get<bool>();
        queued.display_block = ev.contains("display_block")
                                   ? ev.at("display_block").get<int>()
                                   : TurnOrchestrator::roll_inspection_block();
        break;
      case Kind::MayorAward:
        queued.award.type =
            static_cast<MayorAwardType>(ev.at("award_type").get<int>());
        queued.award.announces_winner = ev.contains("announces_winner") &&
                                        ev.at("announces_winner").get<bool>();
        if (ev.contains("winner_player_id") &&
            !ev.at("winner_player_id").is_null()) {
          queued.award.winner_player_id = ev.at("winner_player_id").get<int>();
        }
        break;
      case Kind::DepotRaid:
        break;
      }
      state.turn().restore_deferred_call(queued);
    }
  }

  // Restore deferred protection racket offers
  if (j.contains("deferred_racket_offers")) {
    for (const auto& ev : j.at("deferred_racket_offers")) {
      state.turn().restore_deferred_racket_offer(
          {.player_id = core::PlayerId{ev.at("player_id").get<int>()},
           .restaurant_id =
               core::RestaurantId{ev.at("restaurant_id").get<int>()},
           .first_contact = ev.at("first_contact").get<bool>()});
    }
  }

  // Restore shown message history (clear slate first, then load from save)
  if (j.contains("shown_messages")) {
    for (const auto& [pid_str, entries_json] : j.at("shown_messages").items()) {
      auto pid = core::PlayerId{std::stoi(pid_str)};
      state.career_log().clear_player_messages(pid);
      std::vector<core::events::CareerLog::CvEntry> entries;
      for (const auto& val : entries_json) {
        auto id =
            static_cast<core::events::MilestoneId>(val.at("id").get<int>());
        GameDate date =
            save_date(val.at("y").get<int>(), val.at("m").get<int>(),
                      val.at("d").get<int>());
        entries.push_back({date, id, val.value("shown", true)});
      }
      state.career_log().restore_scheduled(pid, entries);
    }
  }

  if (j.contains("deferred_manager_alerts")) {
    for (const auto& alert : j.at("deferred_manager_alerts")) {
      state.turn().restore_deferred_manager_alert(
          {.restaurant_id =
               core::RestaurantId{alert.at("restaurant_id").get<int>()},
           .message = alert.at("message").get<std::string>(),
           .display_block = alert.at("display_block").get<int>()});
    }
  }

  state.turn().floor_editing_bonus_shown(
      j.value("floor_editing_bonus_shown", false));
  state.competitions().competition_count(j.value("competition_count", 0));
  state.price_update_category(j.value("price_update_category", 0));
  // Saves from before the rate moved onto City hold one rate for every city.
  if (j.contains("global_interest_rate")) {
    int rate = j.at("global_interest_rate").get<int>();
    for (auto* city : state.initialized_cities()) {
      city->interest_rate(rate);
    }
  }
  int phase = j.value("interruption_phase", 0);
  if (!is_valid_interruption_phase(phase)) {
    Logger::warn("Save has interruption phase {}; starting the round-robin "
                 "again",
                 phase);
    phase = 0;
  }
  state.interruption_phase(static_cast<InterruptionPhase>(phase));
  state.second_in_block(j.value("second_in_block", 0));
  state.pit_tick_in_second(j.value("pit_tick_in_second", 0));
  state.pending_screen_charge(j.value("pending_screen_charge_blocks", 0));

  // Save format version 3 kept one list for the whole world; it belongs to the
  // city the player is in.
  if (j.contains("refused_call_actors")) {
    if (City* city = state.active_city()) {
      for (const auto& actor_id : j.at("refused_call_actors")) {
        city->refuse_calls_from(core::ActorId{actor_id.get<int>()});
      }
    }
  }

  if (j.contains("player_cities")) {
    const auto& cities = j.at("player_cities");
    for (int i = 0;
         i < NUMBER_OF_PLAYERS && i < static_cast<int>(cities.size()); ++i) {
      const auto& city = cities.at(static_cast<size_t>(i));
      if (city.is_null()) {
        state.player_city(core::PlayerId{i}, std::nullopt);
      }
      else {
        const core::CityId city_id{city.get<int>()};
        if (!is_valid_city_id(city_id)) {
          throw std::out_of_range("player " + std::to_string(i) +
                                  " is in city " +
                                  std::to_string(city_id.value));
        }
        state.player_city(core::PlayerId{i}, city_id);
      }
    }
  }

  // Last: building the cities and restaurants above draws from the generator,
  // and the loaded game has to resume on the saved sequence.
  if (j.contains("rng_state")) {
    engine::random::set_state(j.at("rng_state").get<uint32_t>());
  }
}

static constexpr std::array<char, 6> HIGH_SCORE_MAGIC = {'P', 'L', 'H',
                                                         'I', 'G', 'H'};
static constexpr std::string_view HIGH_SCORE_FILENAME = "highscore.dat";

std::vector<HighScoreEntry> SaveGameManager::load_high_scores() {
  const fs::path path = get_full_path(std::string(HIGH_SCORE_FILENAME));
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return {}; // No high scores yet
    }

    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    file.seekg(0, std::ios::beg);
    if (!file.good() ||
        end <= static_cast<std::streamoff>(sizeof(SaveHeader)) ||
        static_cast<std::uint64_t>(end) > MAX_HIGH_SCORE_FILE_SIZE) {
      Logger::warn("High score file {} has a bad size", display(path));
      return {};
    }

    std::vector<unsigned char> compressed(static_cast<size_t>(end));
    if (!file.read(reinterpret_cast<char*>(compressed.data()),
                   static_cast<std::streamsize>(compressed.size()))) {
      Logger::warn("Cannot read high score file {}", display(path));
      return {};
    }

    SaveHeader hdr;
    std::memcpy(&hdr, compressed.data(), sizeof(hdr));
    if (std::memcmp(hdr.magic, HIGH_SCORE_MAGIC.data(), 6) != 0) {
      Logger::warn("High score file has invalid magic");
      return {};
    }

    uint64_t decompressed_size = endian::from_le64(hdr.size);
    if (decompressed_size == 0 ||
        decompressed_size > MAX_HIGH_SCORE_JSON_SIZE) {
      Logger::warn("High score file {} claims {} bytes of scores",
                   display(path), decompressed_size);
      return {};
    }

    std::string json_str(static_cast<size_t>(decompressed_size), '\0');
    auto dest_len = static_cast<uLongf>(decompressed_size);
    int zret =
        uncompress(reinterpret_cast<Bytef*>(json_str.data()), &dest_len,
                   compressed.data() + sizeof(SaveHeader),
                   static_cast<uLong>(compressed.size() - sizeof(SaveHeader)));
    if (zret != Z_OK || dest_len != static_cast<uLongf>(decompressed_size)) {
      Logger::warn("High score file {} is damaged", display(path));
      return {};
    }

    auto j =
        parse_bounded(json_str, MAX_HIGH_SCORE_DEPTH, MAX_HIGH_SCORE_VALUES);
    std::vector<HighScoreEntry> entries;
    for (const auto& e : j.value("entries", json::array())) {
      if (static_cast<int>(entries.size()) >= HIGH_SCORE_MAX_ENTRIES) {
        break;
      }
      entries.push_back({
          .name = e.value("name", ""),
          .score = e.value("score", 0),
      });
    }
    return entries;
  }
  catch (const std::exception& e) {
    Logger::warn("Cannot read high scores from {}: {}", display(path),
                 e.what());
    return {};
  }
}

bool SaveGameManager::add_high_score(HighScoreEntry entry) {
  const fs::path path = get_full_path(std::string(HIGH_SCORE_FILENAME));
  try {
    auto entries = load_high_scores();
    entries.push_back(std::move(entry));

    std::ranges::stable_sort(
        entries, [](const HighScoreEntry& a, const HighScoreEntry& b) {
          return a.score > b.score;
        });
    if (static_cast<int>(entries.size()) > HIGH_SCORE_MAX_ENTRIES) {
      entries.resize(HIGH_SCORE_MAX_ENTRIES);
    }

    json j;
    j["version"] = 1;
    j["entries"] = json::array();
    for (const auto& e : entries) {
      j["entries"].push_back({{"name", e.name}, {"score", e.score}});
    }
    std::string json_str = j.dump(-1);

    uLongf bound = compressBound(static_cast<uLong>(json_str.size()));
    std::string compressed(sizeof(SaveHeader) + bound, '\0');
    uLongf compressed_size = bound;
    int zret = compress2(
        reinterpret_cast<Bytef*>(compressed.data() + sizeof(SaveHeader)),
        &compressed_size, reinterpret_cast<const Bytef*>(json_str.data()),
        static_cast<uLong>(json_str.size()), Z_DEFAULT_COMPRESSION);
    if (zret != Z_OK) {
      Logger::warn("High score not saved: compression failed (zlib error {})",
                   zret);
      return false;
    }
    compressed.resize(sizeof(SaveHeader) + compressed_size);

    SaveHeader hdr;
    std::memcpy(hdr.magic, HIGH_SCORE_MAGIC.data(), 6);
    hdr.version = endian::to_le16(HIGH_SCORE_HEADER_VERSION);
    hdr.size = endian::to_le64(static_cast<uint64_t>(json_str.size()));
    std::memcpy(compressed.data(), &hdr, sizeof(hdr));

    if (const std::error_code ec = create_parent_folder(path)) {
      Logger::warn("High score not saved: cannot create the folder for {}: {}",
                   display(path), ec.message());
      return false;
    }
    const util::AtomicWriteResult written =
        util::write_file_atomically(path, compressed);
    if (!written) {
      Logger::warn("High score not saved: {}", written.error);
      return false;
    }
    return true;
  }
  catch (const std::exception& e) {
    Logger::warn("High score not saved to {}: {}", display(path), e.what());
    return false;
  }
}

StateFingerprint SaveGameManager::fingerprint(const GameState& state,
                                              const GameContext& context) {
  json document = state_to_json(state, context);
  document.erase("saved_at");

  // nlohmann::json orders objects by key, so the same state always dumps to the
  // same bytes.
  const std::string bytes = document.dump();

  // FNV-1a
  constexpr uint64_t fnv_offset_basis = 14695981039346656037ULL;
  constexpr uint64_t fnv_prime = 1099511628211ULL;

  uint64_t hash = fnv_offset_basis;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= fnv_prime;
  }

  return StateFingerprint{.hash = compat::format("{:016x}", hash),
                          .document = std::move(document)};
}

} // namespace save
