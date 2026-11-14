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
#include "core/content/ScenarioSave.h"
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
#include "core/save/SaveGameEnums.h"
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
constexpr std::string_view save_unverified_message =
    "The game could not be saved: the new save did not pass its checks. "
    "Your earlier saves are kept.";
constexpr std::string_view checkpoint_failed_notice =
    "The game in progress could not be kept as a checkpoint.";
constexpr std::string_view other_version_status =
    "Save damaged: an earlier verified version was loaded";
constexpr std::string_view restore_nothing_message =
    "This save keeps no versions to restore.";
constexpr std::string_view restore_not_working_message =
    "That version of the save is not a verified working version.";
constexpr std::string_view restore_failed_message =
    "That version of the save failed its check, so nothing was restored.";
constexpr std::string_view restore_unwritable_message =
    "The version could not be restored, because the save folder cannot be "
    "written to. Nothing was changed.";
constexpr std::string_view branch_exists_message =
    "There is already a save of that name.";
constexpr std::string_view undo_nothing_message =
    "There is no restore of this save to undo.";
constexpr std::string_view delete_failed_message =
    "The save could not be deleted.";
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
  // A location outside the original ten is named by its embedded definition
  // (location-contract.md section 6.2); Classic saves get no such member.
  const int active_city = meta["active_city"].get<int>();
  if (const auto it = j.find("scenario");
      it != j.end() && it->is_object() && active_city >= 0) {
    const auto roster = it->find("roster");
    if (roster != it->end() && roster->is_array() &&
        static_cast<std::size_t>(active_city) < roster->size()) {
      const json& entry = (*roster)[static_cast<std::size_t>(active_city)];
      const auto definition = entry.find("definition");
      if (definition != entry.end() && definition->is_object()) {
        const auto display = definition->find("display");
        if (display != definition->end() && display->is_object()) {
          const auto name = display->find("name");
          if (name != display->end() && name->is_string()) {
            meta["active_city_name"] = *name;
          }
        }
      }
    }
  }
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

// Where a save's parts are, from its header.
struct SaveFrame {
  std::uint64_t json_size = 0;      // the game data, inflated
  std::uint64_t payload_offset = 0; // where the zlib stream starts
};

// Checks a save's header and every size in it against the bytes there are.
std::optional<SaveFrame> read_save_frame(std::string_view bytes,
                                         const std::string& name,
                                         LoadProblem& problem) {
  if (bytes.size() > MAX_SAVE_FILE_SIZE) {
    problem.detail = name + " is " + std::to_string(bytes.size()) +
                     " bytes, more than a save can be";
    return std::nullopt;
  }
  SaveHeader hdr;
  if (bytes.size() < sizeof(hdr)) {
    problem.detail = name + " is too short to be a save";
    return std::nullopt;
  }
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
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

  SaveFrame frame;
  frame.json_size = endian::from_le64(hdr.size);
  if (frame.json_size == 0 || frame.json_size > MAX_SAVE_JSON_SIZE) {
    problem.detail = name + " claims " + std::to_string(frame.json_size) +
                     " bytes of game data";
    return std::nullopt;
  }

  frame.payload_offset = sizeof(SaveHeader);
  if (header_version >= 2) {
    uint32_t meta_len = 0;
    if (bytes.size() < sizeof(SaveHeader) + sizeof(meta_len)) {
      problem.detail = name + " ends inside its header";
      return std::nullopt;
    }
    std::memcpy(&meta_len, bytes.data() + sizeof(SaveHeader), sizeof(meta_len));
    meta_len = endian::from_le32(meta_len);
    if (meta_len > MAX_METADATA_SIZE) {
      problem.detail =
          name + " has a " + std::to_string(meta_len) + " byte metadata block";
      return std::nullopt;
    }
    frame.payload_offset += sizeof(meta_len) + meta_len;
  }
  if (frame.payload_offset >= bytes.size()) {
    problem.detail = name + " has no game data after its header";
    return std::nullopt;
  }
  return frame;
}

// Decodes a save's bytes: checks the header and every size in it before
// allocating for them, then inflates and parses the game data.
std::optional<json> decode_save_bytes(std::string_view bytes,
                                      const std::string& name,
                                      LoadProblem& problem) {
  const std::optional<SaveFrame> frame = read_save_frame(bytes, name, problem);
  if (!frame) {
    return std::nullopt;
  }
  const std::string_view compressed =
      bytes.substr(static_cast<std::size_t>(frame->payload_offset));

  std::string json_str(static_cast<size_t>(frame->json_size), '\0');
  auto dest_len = static_cast<uLongf>(frame->json_size);
  int zret = uncompress(reinterpret_cast<Bytef*>(json_str.data()), &dest_len,
                        reinterpret_cast<const Bytef*>(compressed.data()),
                        static_cast<uLong>(compressed.size()));
  if (zret != Z_OK) {
    problem.detail = name + ": the game data does not inflate (zlib error " +
                     std::to_string(zret) + ")";
    return std::nullopt;
  }
  if (dest_len != static_cast<uLongf>(frame->json_size)) {
    problem.detail = name + ": the game data inflates to " +
                     std::to_string(dest_len) + " bytes, not the " +
                     std::to_string(frame->json_size) + " its header claims";
    return std::nullopt;
  }

  try {
    return parse_bounded(json_str, MAX_SAVE_JSON_DEPTH, MAX_SAVE_JSON_VALUES);
  }
  catch (const std::exception& e) {
    problem.detail = name + ": the game data is not valid JSON: " + e.what();
    return std::nullopt;
  }
}

// Reads a whole save file, refusing one larger than any save before reading.
std::optional<std::string> read_save_bytes(const fs::path& path,
                                           LoadProblem& problem) {
  FileRead read = read_whole_file(path, MAX_SAVE_FILE_SIZE);
  if (!read.bytes) {
    problem = {display(path) + ": " + read.problem,
               read.too_large ? load_damaged_message : load_unreadable_message};
  }
  return std::move(read.bytes);
}

std::optional<json> decode_save_file(const fs::path& path,
                                     LoadProblem& problem) {
  const std::optional<std::string> bytes = read_save_bytes(path, problem);
  if (!bytes) {
    return std::nullopt;
  }
  return decode_save_bytes(*bytes, display(path), problem);
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
  // A version 4 save is newer than a Classic one, not older: it keeps its
  // number, and with it its scenario block.
  if (version < SAVE_FORMAT_VERSION) {
    j["version"] = SAVE_FORMAT_VERSION;
  }
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

// A game always has all its players: a new game fills every slot, and a
// player who drops out stays in theirs. The turn code and the start of a
// city's day count and walk them without checking for an empty slot, and
// they count the people first: those are the first slots, then the
// computer's.
void check_players(const json& j) {
  const json& players =
      check_array(require_member(j, "players"), "players", NUMBER_OF_PLAYERS);
  if (players.size() != NUMBER_OF_PLAYERS) {
    reject("players has " + std::to_string(players.size()) + " entries, not " +
           std::to_string(NUMBER_OF_PLAYERS));
  }
  bool computer_seen = false;
  for (std::size_t i = 0; i < players.size(); ++i) {
    const json& player = players[i];
    const std::string what = item("players", i);
    if (player.is_null()) {
      reject(what + " is empty");
    }
    check_object(player, what);
    // The loader puts each player at its place in the list.
    if (check_int(require_member(player, "id", what), 0, NUMBER_OF_PLAYERS - 1,
                  member_name(what, "id")) != static_cast<int>(i)) {
      reject(member_name(what, "id") + " does not match its place");
    }
    const json& human = require_member(player, "human_controlled", what);
    check_bool(human, member_name(what, "human_controlled"));
    if (!human.get<bool>()) {
      computer_seen = true;
    }
    else if (computer_seen) {
      reject(what + " is a person after a computer player");
    }
    check_string(require_member(player, "name", what),
                 member_name(what, "name"));
    const std::string character_name = member_name(what, "character");
    const json& character =
        check_object(require_member(player, "character", what), character_name);
    check_int(require_member(character, "id", character_name), 0, 255,
              member_name(character_name, "id"));
  }
  if (!players[0].at("human_controlled").get<bool>()) {
    reject("players has no person playing");
  }

  // The game runs as the active player from the first frame.
  check_int(require_member(j, "active_player"), 0, NUMBER_OF_PLAYERS - 1,
            "active_player");
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
  // Every save names the city the game is in. Out of range is allowed: the
  // loader falls back to a city it has.
  check_int(require_member(j, "active_city"), int_min, int_max, "active_city");
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

// A game outside the original ten is a version 4 save with its scenario
// block; a Classic one is version 3 or older and has none. What the block
// holds is checked when it is read, while building.
void check_scenario(const json& j) {
  const bool expanded =
      require_member(j, "version") == core::content::expanded_save_version;
  const json* block = find_member(j, "scenario");
  if (expanded && block == nullptr) {
    reject("a version " + std::to_string(core::content::expanded_save_version) +
           " save has no scenario");
  }
  if (!expanded && block != nullptr) {
    reject("a Classic save has a scenario");
  }
  if (block == nullptr) {
    return;
  }
  check_object(*block, "scenario");

  // Every embedded definition is read, validated and held to its hash here,
  // so a damaged Expanded save is never offered as a valid one. Building
  // reads the block again.
  std::string problem;
  const std::optional<core::content::SavedScenario> saved =
      core::content::read_scenario_block(*block, problem);
  if (!saved) {
    reject("the scenario is damaged: " + problem);
  }
  if (saved->roster.size() > static_cast<std::size_t>(NUMBER_OF_CITIES)) {
    reject("the save has " + std::to_string(saved->roster.size()) +
           " locations; this build plays at most " +
           std::to_string(NUMBER_OF_CITIES));
  }

  // CityId i is roster entry i, so every city the save names has to be one
  // of the roster's. Building checks it again against the roster it reads.
  const json* roster = find_member(*block, "roster");
  if (roster == nullptr || !roster->is_array()) {
    reject("the scenario has no roster");
  }
  const auto places = static_cast<std::int64_t>(roster->size());
  const auto check_place = [places](const json& value,
                                    const std::string& what) {
    if (value.is_number_integer() && value.get<std::int64_t>() >= places) {
      reject(what + " is a city this game is not played in");
    }
  };
  if (const json* cities = find_member(j, "cities")) {
    for (std::size_t i = 0; i < cities->size(); ++i) {
      check_place(require_member((*cities)[i], "id"), item("cities", i));
    }
  }
  if (const json* restaurants = find_member(j, "restaurants")) {
    for (std::size_t i = 0; i < restaurants->size(); ++i) {
      check_place(require_member((*restaurants)[i], "property_city_id"),
                  item("restaurants", i));
    }
  }
  if (const json* cities = find_member(j, "player_cities")) {
    for (std::size_t i = 0; i < cities->size(); ++i) {
      check_place((*cities)[i], item("player_cities", i));
    }
  }
  check_place(require_member(j, "active_city"), "active_city");
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
  check_scenario(j);
}

// Reads, upgrades and checks a save. Returns the document ready for
// json_to_state, or nullopt with the problem filled in.
std::optional<json> read_save_document(std::string_view bytes,
                                       const std::string& name,
                                       LoadProblem& problem) {
  try {
    std::optional<json> document = decode_save_bytes(bytes, name, problem);
    if (!document) {
      return std::nullopt;
    }
    const std::optional<int> version = document_version(*document);
    if (!version) {
      problem.detail = name + " has no save format version";
      return std::nullopt;
    }
    if (*version > core::content::expanded_save_version) {
      problem = {name + " is save format " + std::to_string(*version) +
                     "; this build reads up to " +
                     std::to_string(core::content::expanded_save_version),
                 load_newer_message};
      return std::nullopt;
    }
    if (*version < OLDEST_LOADABLE_SAVE_VERSION) {
      problem.detail = name + " is save format " + std::to_string(*version) +
                       ", which no build reads";
      return std::nullopt;
    }
    migrate_save(*document, *version);
    validate_save_document(*document);
    return document;
  }
  catch (const std::exception& e) {
    problem = {name + ": " + e.what(), load_damaged_message};
    return std::nullopt;
  }
}

std::optional<json> read_save_document(const fs::path& path,
                                       LoadProblem& problem) {
  const std::optional<std::string> bytes = read_save_bytes(path, problem);
  if (!bytes) {
    return std::nullopt;
  }
  return read_save_document(*bytes, display(path), problem);
}

// FNV-1a over the bytes, as 16 hex digits.
std::string fingerprint_hash(std::string_view bytes) {
  constexpr uint64_t fnv_offset_basis = 14695981039346656037ULL;
  constexpr uint64_t fnv_prime = 1099511628211ULL;
  uint64_t hash = fnv_offset_basis;
  for (const unsigned char byte : bytes) {
    hash ^= byte;
    hash *= fnv_prime;
  }
  return compat::format("{:016x}", hash);
}

// The integrity check of a version just written: the file holds exactly the
// bytes written and its header holds up. Returns the bytes, or nothing with
// the reason.
std::optional<std::string> check_integrity(const fs::path& file,
                                           const FileDigest& written,
                                           std::string& reason) {
  FileRead read = read_whole_file(file, MAX_SAVE_FILE_SIZE);
  if (!read.bytes) {
    reason = "it can't be read back: " + read.problem;
    return std::nullopt;
  }
  if (read.bytes->size() != written.size) {
    reason = "it is " + std::to_string(read.bytes->size()) +
             " bytes on disk, not the " + std::to_string(written.size) +
             " written";
    return std::nullopt;
  }
  if (checksum_of(*read.bytes) != written.checksum) {
    reason = "its bytes on disk are not the ones written";
    return std::nullopt;
  }
  LoadProblem problem;
  if (!read_save_frame(*read.bytes, display(file), problem)) {
    reason = problem.detail;
    return std::nullopt;
  }
  return std::move(read.bytes);
}

// Finishes retiring: removes the files of the versions the manifest dropped.
// One that can't go yet stays listed for the next save to try again.
void remove_retired(const SaveSlot& slot, SlotManifest& manifest) {
  std::erase_if(manifest.retired, [&slot](std::uint64_t sequence) {
    const fs::path file = slot.version_path(sequence);
    std::error_code ec;
    fs::remove(file, ec);
    if (ec) {
      Logger::warn("Could not remove {}: {}", display(file), ec.message());
      return false;
    }
    return true;
  });
}

// One copy of a save that loading may try, with what it must hold if that
// is known.
struct LoadCopy {
  fs::path file;
  std::optional<FileDigest> expected;
  std::optional<SaveVersion> version;
};

// The copies to try, in order. A slot without versions has its file. A slot
// with versions has its working versions, the active one first and then the
// newest; each from its own file, then from the slot's file when that holds
// the same bytes. A slot's file that matches no version was written outside
// them (by hand, by save() or by an older build), so it goes first. Version
// files the manifest does not list come last.
std::vector<LoadCopy> plan_load(const fs::path& path) {
  const SaveSlot slot(path);
  std::vector<LoadCopy> copies;
  if (!slot.has_versions()) {
    copies.push_back(
        {.file = path, .expected = std::nullopt, .version = std::nullopt});
    return copies;
  }

  const SaveSlot::ManifestRead read = slot.read_manifest();
  std::error_code ec;
  const bool slot_file_there = fs::exists(path, ec);
  if (!read.manifest) {
    if (!read.problem.empty()) {
      Logger::warn("The versions of {} can't be used: {}", display(path),
                   read.problem);
    }
    if (slot_file_there) {
      copies.push_back(
          {.file = path, .expected = std::nullopt, .version = std::nullopt});
    }
    for (const std::uint64_t sequence : slot.version_files()) {
      copies.push_back({.file = slot.version_path(sequence),
                        .expected = std::nullopt,
                        .version = std::nullopt});
    }
    return copies;
  }

  const SlotManifest& manifest = *read.manifest;
  std::optional<FileDigest> slot_digest;
  if (slot_file_there) {
    const FileRead bytes = read_whole_file(path, MAX_SAVE_FILE_SIZE);
    if (bytes.bytes) {
      slot_digest = digest_of(*bytes.bytes);
    }
  }
  const auto digest = [](const SaveVersion& v) {
    return FileDigest{.size = v.size, .checksum = v.checksum};
  };
  const bool slot_listed =
      slot_digest &&
      std::ranges::any_of(manifest.versions, [&](const SaveVersion& v) {
        return digest(v) == *slot_digest;
      });
  if (slot_file_there && !slot_listed) {
    copies.push_back(
        {.file = path, .expected = std::nullopt, .version = std::nullopt});
  }

  std::vector<const SaveVersion*> working;
  for (const SaveVersion& v : manifest.versions) {
    if (v.status == VersionStatus::Working) {
      working.push_back(&v);
    }
  }
  std::ranges::stable_partition(working, [&](const SaveVersion* v) {
    return manifest.active && *manifest.active == v->sequence;
  });
  for (const SaveVersion* v : working) {
    copies.push_back({.file = slot.version_path(v->sequence),
                      .expected = digest(*v),
                      .version = *v});
    if (slot_digest == digest(*v)) {
      copies.push_back({.file = path, .expected = digest(*v), .version = *v});
    }
  }

  for (const std::uint64_t sequence : slot.version_files()) {
    if (manifest.find(sequence) == nullptr &&
        std::ranges::find(manifest.retired, sequence) ==
            manifest.retired.end()) {
      copies.push_back({.file = slot.version_path(sequence),
                        .expected = std::nullopt,
                        .version = std::nullopt});
    }
  }
  return copies;
}

// "2026-11-14 09:30 UTC (Mar 4, 1995)", for the player.
std::string when_saved(const SaveVersion& v) {
  std::string when = v.written_at;
  if (when.size() >= 16) {
    when = when.substr(0, 10) + " " + when.substr(11, 5) + " UTC";
  }
  if (!v.game_date.empty()) {
    when += " (" + v.game_date + ")";
  }
  return when;
}

// What the player is told when loading took another copy than the first.
// Nothing when it was the same version from its other copy.
std::string other_copy_notice(const LoadCopy& first, const LoadCopy& loaded) {
  if (first.version && loaded.version &&
      first.version->sequence == loaded.version->sequence) {
    return {};
  }
  if (loaded.version) {
    return "The save could not be loaded as it was, so the game loaded the "
           "verified version of it saved " +
           when_saved(*loaded.version) + ".";
  }
  return "The save could not be loaded as it was, so the game loaded an "
         "older copy of it.";
}

// Whether a game is in progress: every player there, a person first. The
// menus' stand-in players are all computer players.
bool game_in_progress(const GameState& state) {
  const auto players = state.players();
  return std::ranges::all_of(players,
                             [](const Player* p) { return p != nullptr; }) &&
         players[0]->is_human_controlled();
}

// A save ready to be written as a version.
struct NewVersion {
  std::string bytes;
  std::string expected; // the fingerprint of the game saved
  std::string game_date;
  std::string made_for;
};

// The bytes of a captured game, and what its version records.
NewVersion prepare_version(json document, std::string_view made_for) {
  NewVersion version;
  version.bytes = encode_save(document);
  version.game_date = metadata_from_save_json(document).value("game_date", "");
  document.erase("saved_at");
  version.expected = fingerprint_hash(document.dump());
  version.made_for = made_for;
  return version;
}

// A version written and integrity-checked, but not committed.
struct Staged {
  SlotManifest manifest;
  std::uint64_t sequence = 0;
  std::optional<std::string> on_disk; // its bytes read back, if they passed
  std::string reason;                 // why they did not
  std::error_code code;               // why staging failed
  bool newer = false;                 // a newer build keeps the slot's versions
};

// Steps 1 to 3 of a save to a slot:
// 1. list the new version as a candidate "being written", so a save cut
//    short anywhere leaves nothing the manifest does not account for;
// 2. write it under a name of its own, touching no working version;
// 3. the integrity check: what is on disk is what was written.
// Returns false, having changed no working version, if it could not get as
// far as checking.
bool stage_version(const SaveSlot& slot, const NewVersion& version,
                   Staged& staged) {
  std::error_code ec = create_parent_folder(slot.file());
  if (!ec) {
    fs::create_directories(slot.folder(), ec);
  }
  if (ec) {
    Logger::warn("Save failed: cannot create the folder for {}: {}",
                 display(slot.folder()), ec.message());
    staged.code = ec;
    return false;
  }

  const SaveSlot::ManifestRead read = slot.read_manifest();
  if (read.newer) {
    Logger::warn("A newer build keeps the versions of {} ({}); saving the "
                 "file alone",
                 display(slot.file()), read.problem);
    staged.newer = true;
    return false;
  }
  if (!read.problem.empty()) {
    Logger::warn("The versions of {} start afresh, keeping the files there: "
                 "{}",
                 display(slot.file()), read.problem);
  }
  staged.manifest = read.manifest.value_or(SlotManifest{});
  SlotManifest& manifest = staged.manifest;
  remove_retired(slot, manifest);

  // Past every version file there, listed or not.
  std::uint64_t sequence = manifest.next_sequence;
  for (const std::uint64_t on_disk : slot.version_files()) {
    sequence = std::max(sequence, on_disk + 1);
  }
  staged.sequence = sequence;
  manifest.next_sequence = sequence + 1;
  const FileDigest digest = digest_of(version.bytes);
  manifest.versions.insert(manifest.versions.begin(),
                           SaveVersion{
                               .sequence = sequence,
                               .status = VersionStatus::Candidate,
                               .level = VersionLevel::Written,
                               .reason = "being written",
                               .made_for = version.made_for,
                               .written_at = utc_now(),
                               .size = digest.size,
                               .checksum = digest.checksum,
                               .fingerprint = version.expected,
                               .game_date = version.game_date,
                           });

  if (const util::AtomicWriteResult listed = slot.write_manifest(manifest);
      !listed) {
    Logger::warn("Save failed: {}", listed.error);
    staged.code = listed.code;
    return false;
  }

  const fs::path candidate = slot.version_path(sequence);
  if (const util::AtomicWriteResult written =
          util::write_file_atomically(candidate, version.bytes);
      !written) {
    manifest.find(sequence)->reason =
        "it could not be written: " + written.error;
    static_cast<void>(slot.write_manifest(manifest));
    Logger::warn("Save failed: {}", written.error);
    staged.code = written.code;
    return false;
  }

  staged.on_disk = check_integrity(candidate, digest, staged.reason);
  return true;
}

// Step 5: records how far the new version got, applies retention and
// writes the manifest. A working version becomes the active one.
bool commit_version(const SaveSlot& slot, Staged& staged, VersionLevel level,
                    const std::string& reason, std::string* error) {
  SlotManifest& manifest = staged.manifest;
  SaveVersion& version = *manifest.find(staged.sequence);
  const bool working = level == VersionLevel::LoadVerified;
  version.level = level;
  version.status = working ? VersionStatus::Working : VersionStatus::Candidate;
  version.reason = working ? std::string() : reason;
  if (working) {
    manifest.active = staged.sequence;
  }
  apply_retention(manifest);
  if (const util::AtomicWriteResult committed = slot.write_manifest(manifest);
      !committed) {
    Logger::warn("Save failed: {}", committed.error);
    set_message(error, save_failure_message(committed.code));
    return false;
  }
  return true;
}

// Steps 6 and 7 for a working version: it becomes the slot's file too, for
// the load menu and older builds (if it can't, loading still takes it from
// the versions), and the retired versions go.
void activate(const SaveSlot& slot, SlotManifest& manifest,
              std::string_view bytes) {
  if (const util::AtomicWriteResult refreshed =
          util::write_file_atomically(slot.file(), bytes);
      !refreshed) {
    Logger::warn("{} still holds an earlier version ({}); loading it takes "
                 "the new one from its versions",
                 display(slot.file()), refreshed.error);
  }
  remove_retired(slot, manifest);
}

// Before a new timed checkpoint: the slot's file catches up with the
// version promoted since, which the game thread left to the worker so that
// promoting stays one small write.
void bring_slot_file_in_step(const SaveSlot& slot) {
  const SaveSlot::ManifestRead read = slot.read_manifest();
  if (!read.manifest || !read.manifest->active) {
    return;
  }
  const SaveVersion& active = *read.manifest->find(*read.manifest->active);
  const FileDigest wanted{.size = active.size, .checksum = active.checksum};
  const FileRead current = read_whole_file(slot.file(), MAX_SAVE_FILE_SIZE);
  if (current.bytes && digest_of(*current.bytes) == wanted) {
    return;
  }
  const FileRead bytes =
      read_whole_file(slot.version_path(active.sequence), MAX_SAVE_FILE_SIZE);
  if (!bytes.bytes || digest_of(*bytes.bytes) != wanted) {
    return; // loading takes it from the versions anyway
  }
  if (const util::AtomicWriteResult refreshed =
          util::write_file_atomically(slot.file(), *bytes.bytes);
      !refreshed) {
    Logger::warn("{} still holds an earlier version ({})", display(slot.file()),
                 refreshed.error);
  }
}

// The worker's half of a background save: fingerprint the captured game,
// stop if the slot's newest version (working or waiting) already holds it
// and coalesce is set, else write the version and check it every way, the
// build included. A version that passed waits for the game thread to
// promote it, with the slot's file already written; one that failed stays a
// candidate with the reason.
void write_background_version(const fs::path& path, const NewVersion& version,
                              bool coalesce,
                              const std::function<std::optional<std::string>(
                                  std::string_view, const std::string&)>& build,
                              CheckpointJob::Shared& shared) {
  const SaveSlot slot(path);
  bring_slot_file_in_step(slot);
  if (coalesce) {
    const SaveSlot::ManifestRead read = slot.read_manifest();
    if (read.manifest) {
      for (const SaveVersion& v : read.manifest->versions) {
        if (v.status == VersionStatus::Working ||
            v.reason == awaiting_promotion) {
          shared.unchanged = v.fingerprint == version.expected;
          break;
        }
      }
    }
    if (shared.unchanged) {
      return;
    }
  }

  Staged staged;
  if (!stage_version(slot, version, staged)) {
    shared.error = staged.newer
                       ? "a newer build keeps these versions"
                       : "it could not be written: " + staged.code.message();
    return;
  }
  const std::string name = display(slot.version_path(staged.sequence));
  std::string reason = staged.reason;
  VersionLevel level = VersionLevel::Written;
  if (staged.on_disk) {
    level = VersionLevel::IntegrityChecked;
    if (std::optional<std::string> why = build(*staged.on_disk, name)) {
      reason = std::move(*why);
    }
    else {
      level = VersionLevel::LoadVerified;
      reason = awaiting_promotion;
    }
  }
  SaveVersion& written = *staged.manifest.find(staged.sequence);
  written.level = level;
  written.status = VersionStatus::Candidate;
  written.reason = reason;
  apply_retention(staged.manifest);
  if (const util::AtomicWriteResult committed =
          slot.write_manifest(staged.manifest);
      !committed) {
    shared.error = "its manifest could not be written: " + committed.error;
    return;
  }
  shared.sequence = staged.sequence;
  shared.ready = level == VersionLevel::LoadVerified;
  if (!shared.ready) {
    shared.error = reason;
    return;
  }
  // The slot's file now, so promoting stays one small write. Until then,
  // loading still takes the active version: this one is listed, and only
  // as a candidate.
  if (const util::AtomicWriteResult refreshed =
          util::write_file_atomically(slot.file(), version.bytes);
      !refreshed) {
    Logger::warn("{} still holds an earlier version ({})", display(slot.file()),
                 refreshed.error);
  }
}

// The background autosaves still running or not yet promoted. The game
// thread alone touches the list.
std::vector<std::unique_ptr<CheckpointJob>>& background_autosaves() {
  static std::vector<std::unique_ptr<CheckpointJob>> jobs;
  return jobs;
}

// Finishes the background autosaves to path, if any, before the game thread
// touches that slot itself.
void wait_for_background(const fs::path& path) {
  auto& jobs = background_autosaves();
  std::erase_if(jobs, [&path](const std::unique_ptr<CheckpointJob>& job) {
    if (job->slot_file() != path) {
      return false;
    }
    job->wait();
    static_cast<void>(SaveGameManager::promote(*job));
    return true;
  });
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

std::string next_autosave_filename(int slot_count,
                                   const fs::path& folder = {}) {
  const fs::path saves_dir =
      folder.empty() ? fs::path(platform_paths::saves_dir()) : folder;

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
  bool ok =
      save_version(state, context, get_full_path(filename), "autosave", error);
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
    meta.active_city_name = m.value("active_city_name", std::string{});

    return meta;
  }
  catch (const std::exception& e) {
    Logger::warn("Cannot read save details from {}: {}", filename, e.what());
    return std::nullopt;
  }
}

bool SaveGameManager::load(GameState& state, GameContext& context,
                           const std::string& filename, std::string* error,
                           std::string* notice) {
  return load_slot(state, context, filename, error, notice, {});
}

bool SaveGameManager::load_over_current(GameState& state, GameContext& context,
                                        const std::string& filename,
                                        std::string* error, std::string* notice,
                                        const fs::path& checkpoint_slot) {
  bool kept = true;
  const bool loaded = load_slot(state, context, filename, error, notice, [&] {
    kept = checkpoint(state, context, CheckpointReason::BeforeLoad,
                      checkpoint_slot) != CheckpointResult::Failed;
  });
  if (!kept && notice != nullptr) {
    *notice = notice->empty()
                  ? std::string(checkpoint_failed_notice)
                  : std::string(checkpoint_failed_notice) + " " + *notice;
  }
  return loaded;
}

bool SaveGameManager::load_slot(GameState& state, GameContext& context,
                                const std::string& filename, std::string* error,
                                std::string* notice,
                                const std::function<void()>& before_replacing) {
  Logger::info("Loading game from {}", filename);
  const fs::path path = get_full_path(filename);
  wait_for_background(path);
  const std::vector<LoadCopy> copies = plan_load(path);

  enum class Built { Loaded, Rejected, Lost };
  // Builds the game from the save in a scratch state first. Damage the
  // checks can't see, such as a field missing deep inside a restaurant,
  // shows up there while the running game is still untouched. Building draws
  // from the shared random generator, so that is put back afterwards.
  const auto build = [&](const json& document, const fs::path& file,
                         LoadProblem& problem) {
    Logger::info(
        "Checking the save by building it apart from the running game");
    const uint32_t random_state = engine::random::get_state();
    bool built = false;
    std::string build_error;
    try {
      // Set up as the live state is, as far as anything set up survives:
      // json_to_state starts with reset(), which keeps the options (and the
      // presenters and status hub, which the scratch state has none of) and
      // empties the rest. What the game's startup adds on top, a player in
      // every slot (create_placeholder_players), json_to_state requires of
      // the save itself.
      auto scratch = std::make_unique<GameState>();
      scratch->options() = state.options();
      json_to_state(document, *scratch, context);
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
      problem = {display(file) + ": " + build_error, load_damaged_message};
      return Built::Rejected;
    }

    if (before_replacing) {
      before_replacing();
    }

    // Every check passed, so load for real. It is the same build from the
    // same document and the same random state, so it goes as the scratch
    // one did.
    Logger::info("Save checked; loading it into the game");
    try {
      json_to_state(document, state, context);
      state.finalize_initialization(context);

      // Land in the city view for the loaded city
      state.ui().pop_to_root();
      state.ui().replace_top<CityViewState>(state.active_city_id());
    }
    catch (const std::exception& e) {
      Logger::warn("Load of {} failed after its checks passed: {}",
                   display(file), e.what());
      return Built::Lost;
    }
    return Built::Loaded;
  };

  std::string_view message = load_unreadable_message;
  for (std::size_t i = 0; i < copies.size(); ++i) {
    const LoadCopy& copy = copies[i];
    LoadProblem problem;
    std::optional<json> document;
    if (const std::optional<std::string> bytes =
            read_save_bytes(copy.file, problem)) {
      if (copy.expected && digest_of(*bytes) != *copy.expected) {
        problem = {display(copy.file) +
                       " does not match the checksum its versions list",
                   load_damaged_message};
      }
      else {
        document = read_save_document(*bytes, display(copy.file), problem);
      }
    }

    const Built built =
        document ? build(*document, copy.file, problem) : Built::Rejected;
    if (built == Built::Lost) {
      set_message(error, load_failed_message);
      return false;
    }
    if (built == Built::Loaded) {
      if (i > 0) {
        const std::string told = other_copy_notice(copies.front(), copy);
        Logger::warn("Loaded {} instead of {}", display(copy.file),
                     display(copies.front().file));
        if (!told.empty()) {
          state.status().post(StatusMsg{
              .payload = std::string(other_version_status),
              .severity = StatusSeverity::Warning,
          });
          if (notice != nullptr) {
            *notice = told;
          }
        }
      }
      Logger::info("Load completed successfully");
      return true;
    }
    Logger::warn("Save rejected: {}", problem.detail);
    if (i == 0) {
      message = problem.message;
    }
  }
  set_message(error, message);
  return false;
}

bool SaveGameManager::save_to_slot(const GameState& state,
                                   const GameContext& context,
                                   const std::string& filename,
                                   std::string* error) {
  return save_version(state, context, get_full_path(filename), "save", error);
}

std::optional<std::string>
SaveGameManager::verify_build(std::string_view bytes, const std::string& name,
                              const std::string& expected_fingerprint,
                              const GameOptions& options,
                              const GameContext& context) {
  LoadProblem problem;
  const std::optional<json> document = read_save_document(bytes, name, problem);
  if (!document) {
    return problem.detail;
  }
  // As load's scratch build; the random generator is put back after.
  const uint32_t random_state = engine::random::get_state();
  std::optional<std::string> why;
  try {
    auto scratch = std::make_unique<GameState>();
    scratch->options() = options;
    json_to_state(*document, *scratch, context);
    const std::string built = fingerprint(*scratch, context).hash;
    if (built != expected_fingerprint) {
      why = "it loads as another game: fingerprint " + built + ", not " +
            expected_fingerprint;
    }
  }
  catch (const std::exception& e) {
    why = std::string("it does not load: ") + e.what();
  }
  catch (...) {
    why = "it does not load";
  }
  engine::random::set_state(random_state);
  return why;
}

bool SaveGameManager::save_version(const GameState& state,
                                   const GameContext& context,
                                   const fs::path& path,
                                   std::string_view made_for,
                                   std::string* error) {
  Logger::info("Saving game to {}, keeping its versions", display(path));
  wait_for_background(path);
  NewVersion version;
  try {
    version = prepare_version(state_to_json(state, context), made_for);
  }
  catch (const std::exception& e) {
    Logger::warn("Save failed: {}: {}", display(path), e.what());
    set_message(error, save_failed_message);
    return false;
  }

  const SaveSlot slot(path);
  Staged staged;
  if (!stage_version(slot, version, staged)) {
    if (!staged.newer) {
      set_message(error, save_failure_message(staged.code));
      return false;
    }
    // Its versions belong to a newer build; leave them be and write the file
    // alone, as builds before versions did.
    const util::AtomicWriteResult written =
        util::write_file_atomically(path, version.bytes);
    if (!written) {
      Logger::warn("Save failed: {}", written.error);
      set_message(error, save_failure_message(written.code));
      return false;
    }
    return true;
  }

  // 4. The load check: built apart from the game, it is the game saved.
  std::string reason = staged.reason;
  VersionLevel level =
      staged.on_disk ? VersionLevel::IntegrityChecked : VersionLevel::Written;
  if (staged.on_disk) {
    Logger::info("Checking the new save by building it apart from the game");
    if (std::optional<std::string> why = verify_build(
            *staged.on_disk, display(slot.version_path(staged.sequence)),
            version.expected, state.options(), context)) {
      reason = std::move(*why);
    }
    else {
      level = VersionLevel::LoadVerified;
    }
  }
  const bool working = level == VersionLevel::LoadVerified;
  if (!commit_version(slot, staged, level, reason, error)) {
    return false;
  }
  if (!working) {
    Logger::warn("Save failed: version {} of {} did not pass its checks: {}",
                 staged.sequence, display(path), reason);
    set_message(error, save_unverified_message);
    return false;
  }
  activate(slot, staged.manifest, version.bytes);
  Logger::info("Save completed: version {} of {}, load-verified",
               staged.sequence, display(path));
  return true;
}

CheckpointResult SaveGameManager::checkpoint(const GameState& state,
                                             const GameContext& context,
                                             CheckpointReason reason,
                                             const fs::path& slot_file) {
  if (!game_in_progress(state)) {
    return CheckpointResult::NoGame;
  }
  const fs::path path =
      slot_file.empty()
          ? fs::path(get_full_path(std::string(checkpoint_filename)))
          : slot_file;
  try {
    const std::string now = fingerprint(state, context).hash;
    const SaveSlot::ManifestRead read = SaveSlot(path).read_manifest();
    if (read.manifest) {
      const SaveVersion* newest = read.manifest->newest_working();
      if (newest != nullptr && newest->fingerprint == now) {
        Logger::info("The newest checkpoint already holds this game");
        return CheckpointResult::Unchanged;
      }
    }
  }
  catch (const std::exception& e) {
    Logger::warn("Checkpoint failed: {}", e.what());
    return CheckpointResult::Failed;
  }
  const std::string_view made_for = reason == CheckpointReason::BeforeNewGame
                                        ? "checkpoint before a new game"
                                        : "checkpoint before loading a save";
  Logger::info("Keeping the game in progress: {}", made_for);
  return save_version(state, context, path, made_for, nullptr)
             ? CheckpointResult::Written
             : CheckpointResult::Failed;
}

std::optional<SlotManifest>
SaveGameManager::versions(const std::string& filename, std::string* problem) {
  SaveSlot::ManifestRead read =
      SaveSlot(get_full_path(filename)).read_manifest();
  if (problem != nullptr) {
    *problem = read.problem;
  }
  return std::move(read.manifest);
}

bool SaveGameManager::restore_version(const GameState& state,
                                      const GameContext& context,
                                      const std::string& filename,
                                      std::uint64_t sequence,
                                      std::string* error) {
  const fs::path path = get_full_path(filename);
  wait_for_background(path);
  const SaveSlot slot(path);
  SaveSlot::ManifestRead read = slot.read_manifest();
  if (!read.manifest) {
    Logger::warn("Nothing to restore in {}: {}", display(path),
                 read.problem.empty() ? "it keeps no versions" : read.problem);
    set_message(error, restore_nothing_message);
    return false;
  }
  SlotManifest manifest = std::move(*read.manifest);
  SaveVersion* version = manifest.find(sequence);
  if (version == nullptr || version->status != VersionStatus::Working) {
    Logger::warn("Version {} of {} is not a working version", sequence,
                 display(path));
    set_message(error, restore_not_working_message);
    return false;
  }

  // Check it again, apart from the game, before anything changes.
  const fs::path file = slot.version_path(sequence);
  std::string reason;
  const std::optional<std::string> bytes = check_integrity(
      file, FileDigest{.size = version->size, .checksum = version->checksum},
      reason);
  if (bytes) {
    if (std::optional<std::string> why =
            verify_build(*bytes, display(file), version->fingerprint,
                         state.options(), context)) {
      reason = std::move(*why);
    }
  }
  if (!reason.empty()) {
    Logger::warn("Version {} of {} failed its check: {}", sequence,
                 display(path), reason);
    version->status = VersionStatus::Candidate;
    version->level =
        bytes ? VersionLevel::IntegrityChecked : VersionLevel::Written;
    version->reason = "it failed its check before a restore: " + reason;
    if (manifest.active == sequence) {
      const SaveVersion* other = manifest.newest_working();
      manifest.active =
          other ? std::optional<std::uint64_t>(other->sequence) : std::nullopt;
    }
    static_cast<void>(slot.write_manifest(manifest));
    set_message(error, restore_failed_message);
    return false;
  }

  // Activate: the manifest first, then the slot's file. Cut short between
  // the two, loading already takes the restored version.
  if (manifest.active != sequence) {
    manifest.previous_active = manifest.active;
  }
  manifest.active = sequence;
  if (const util::AtomicWriteResult written = slot.write_manifest(manifest);
      !written) {
    Logger::warn("Restore failed: {}", written.error);
    set_message(error, restore_unwritable_message);
    return false;
  }
  if (const util::AtomicWriteResult refreshed =
          util::write_file_atomically(path, *bytes);
      !refreshed) {
    Logger::warn("{} still holds the version it had ({}); loading it takes "
                 "the restored one",
                 display(path), refreshed.error);
  }
  Logger::info("Restored version {} of {}", sequence, display(path));
  return true;
}

namespace {
fs::path timed_slot(const fs::path& slot_file) {
  return slot_file.empty()
             ? fs::path(platform_paths::saves_dir()) / timed_checkpoint_filename
             : slot_file;
}
} // namespace

json SaveGameManager::capture(const GameState& state,
                              const GameContext& context) {
  return state_to_json(state, context);
}

std::unique_ptr<CheckpointJob>
SaveGameManager::start_background(const fs::path& path, json document,
                                  std::string_view made_for,
                                  const GameOptions& options, bool coalesce) {
  auto job = std::make_unique<CheckpointJob>();
  job->slot_file_ = path;
  job->thread_ = std::jthread(
      [shared = job->shared_, path, document = std::move(document),
       made_for = std::string(made_for), options, coalesce]() mutable {
        // The build has this thread's own random streams and reads the
        // factories' tables without locks; it gets a context of its own, as
        // nothing in a build may reach the game's.
        const GameContext own_context;
        try {
          const NewVersion version =
              prepare_version(std::move(document), made_for);
          write_background_version(
              path, version, coalesce,
              [&](std::string_view bytes, const std::string& name) {
                return verify_build(bytes, name, version.expected, options,
                                    own_context);
              },
              *shared);
        }
        catch (const std::exception& e) {
          shared->error = e.what();
        }
        catch (...) {
          // A test's interruption stands for the game being killed here; on a
          // worker it must not end the program instead.
          shared->error = "it was cut short";
        }
        if (!shared->error.empty()) {
          Logger::warn("Background save of {} failed: {}", display(path),
                       shared->error);
        }
        shared->done.store(true);
      });
  return job;
}

std::unique_ptr<CheckpointJob> SaveGameManager::start_timed_checkpoint(
    const GameState& state, const GameContext& context, TimedCheckpoint reason,
    CheckpointResult* result, const fs::path& slot_file) {
  const auto report = [result](CheckpointResult r) {
    if (result != nullptr) {
      *result = r;
    }
  };
  if (!game_in_progress(state)) {
    report(CheckpointResult::NoGame);
    return nullptr;
  }
  const std::string_view made_for =
      reason == TimedCheckpoint::Interval ? "checkpoint after five minutes"
      : reason == TimedCheckpoint::NewDay ? "checkpoint at a new day"
                                          : "checkpoint at a new week";
  // Captured here, at a consistent moment of the game, and nothing else:
  // fingerprinting, comparing and writing run on the worker.
  json document;
  try {
    document = capture(state, context);
  }
  catch (const std::exception& e) {
    Logger::warn("Checkpoint failed: {}", e.what());
    report(CheckpointResult::Failed);
    return nullptr;
  }
  report(CheckpointResult::Written);
  return start_background(timed_slot(slot_file), std::move(document), made_for,
                          state.options(), true);
}

CheckpointResult SaveGameManager::promote(const CheckpointJob& job) {
  if (!job.done() || job.unchanged()) {
    return CheckpointResult::Unchanged;
  }
  if (!job.ready()) {
    return CheckpointResult::Failed;
  }
  const SaveSlot slot(job.slot_file());
  SaveSlot::ManifestRead read = slot.read_manifest();
  SaveVersion* version =
      read.manifest ? read.manifest->find(job.sequence()) : nullptr;
  if (version == nullptr || version->level != VersionLevel::LoadVerified ||
      version->reason != awaiting_promotion) {
    Logger::warn("Version {} of {} is no longer waiting to be promoted",
                 job.sequence(), display(job.slot_file()));
    return CheckpointResult::Failed;
  }
  SlotManifest& manifest = *read.manifest;
  version->status = VersionStatus::Working;
  version->reason.clear();
  manifest.active = job.sequence();
  apply_retention(manifest);
  if (const util::AtomicWriteResult committed = slot.write_manifest(manifest);
      !committed) {
    Logger::warn("Promotion failed: {}", committed.error);
    return CheckpointResult::Failed;
  }
  Logger::info("Version {} of {} is a working version", job.sequence(),
               display(job.slot_file()));
  return CheckpointResult::Written;
}

bool SaveGameManager::autosave_in_background(const GameState& state,
                                             const GameContext& context,
                                             const fs::path& folder) {
  const int slot_count = std::clamp(state.options().autosave_count,
                                    GameOptions::AUTOSAVE_COUNT_MIN,
                                    GameOptions::AUTOSAVE_COUNT_MAX);
  const std::string name = next_autosave_filename(slot_count, folder);
  const fs::path path =
      folder.empty() ? fs::path(get_full_path(name)) : folder / name;
  wait_for_background(path);
  // Where nothing settles every frame (a test, a script), finished jobs
  // would pile up: promote those that passed now. One that failed stays
  // for settle_background_saves to tell the player of.
  std::erase_if(background_autosaves(),
                [](const std::unique_ptr<CheckpointJob>& job) {
                  if (!job->done() || (!job->ready() && !job->unchanged())) {
                    return false;
                  }
                  job->wait();
                  return promote(*job) != CheckpointResult::Failed;
                });
  json document;
  try {
    document = capture(state, context);
  }
  catch (const std::exception& e) {
    Logger::warn("Autosave failed: {}", e.what());
    return false;
  }
  Logger::info("Autosaving to {} in the background", display(path));
  background_autosaves().push_back(start_background(
      path, std::move(document), "autosave", state.options(), false));
  return true;
}

void SaveGameManager::settle_background_saves(GameState& state) {
  std::erase_if(
      background_autosaves(),
      [&state](const std::unique_ptr<CheckpointJob>& job) {
        if (!job->done()) {
          return false;
        }
        job->wait();
        if (promote(*job) == CheckpointResult::Failed) {
          // As the autosave always has: the old one is kept, and
          // the player can save by hand.
          state.status().post(StatusMsg{
              .payload = std::string("Autosave failed! Save by hand."),
              .severity = StatusSeverity::Warning,
          });
        }
        return true;
      });
}

std::size_t SaveGameManager::background_saves() {
  return background_autosaves().size();
}

void SaveGameManager::finish_background_saves() {
  for (const auto& job : background_autosaves()) {
    job->wait();
    static_cast<void>(promote(*job));
  }
  background_autosaves().clear();
}

std::vector<RecoverySlot>
SaveGameManager::recovery_slots(const fs::path& folder) {
  const fs::path where =
      folder.empty() ? fs::path(platform_paths::saves_dir()) : folder;
  std::vector<std::string> names;
  std::error_code ec;
  fs::directory_iterator it(where, ec);
  for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    const std::u8string raw = it->path().filename().u8string();
    std::string name(raw.begin(), raw.end());
    if (it->is_regular_file(entry_ec) && !entry_ec &&
        it->path().extension() == ".plsave") {
      names.push_back(std::move(name));
    }
    else if (it->is_directory(entry_ec) && !entry_ec &&
             is_versions_folder_name(name)) {
      // A slot whose own file is gone still has its versions.
      names.push_back(
          name.substr(0, name.size() - std::string_view(".versions").size()));
    }
  }
  std::ranges::sort(names);
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::vector<RecoverySlot> slots;
  for (const std::string& name : names) {
    const SaveSlot slot(where / name);
    SaveSlot::ManifestRead read = slot.read_manifest();
    std::error_code file_ec;
    slots.push_back(RecoverySlot{
        .name = name,
        .checkpoint =
            name == checkpoint_filename || name == timed_checkpoint_filename,
        .has_file = fs::is_regular_file(slot.file(), file_ec),
        .versions = std::move(read.manifest),
        .problem = std::move(read.problem),
    });
  }
  return slots;
}

bool SaveGameManager::restore_as_branch(const GameState& state,
                                        const GameContext& context,
                                        const std::string& filename,
                                        std::uint64_t sequence,
                                        const std::string& new_filename,
                                        std::string* error) {
  const SaveSlot from(get_full_path(filename));
  const SaveSlot to(get_full_path(new_filename));
  wait_for_background(from.file());
  wait_for_background(to.file());
  std::error_code ec;
  if (fs::exists(to.file(), ec) || fs::exists(to.folder(), ec) ||
      to.file() == from.file()) {
    Logger::warn("Not branching into {}: it is already a save",
                 display(to.file()));
    set_message(error, branch_exists_message);
    return false;
  }
  const SaveSlot::ManifestRead read = from.read_manifest();
  const SaveVersion* version =
      read.manifest ? read.manifest->find(sequence) : nullptr;
  if (version == nullptr || version->status != VersionStatus::Working) {
    set_message(error, read.manifest ? restore_not_working_message
                                     : restore_nothing_message);
    return false;
  }

  // Checked again, apart from the game, before anything is written.
  const fs::path file = from.version_path(sequence);
  std::string reason;
  const std::optional<std::string> bytes = check_integrity(
      file, FileDigest{.size = version->size, .checksum = version->checksum},
      reason);
  if (bytes) {
    if (std::optional<std::string> why =
            verify_build(*bytes, display(file), version->fingerprint,
                         state.options(), context)) {
      reason = std::move(*why);
    }
  }
  if (!reason.empty()) {
    Logger::warn("Version {} of {} failed its check: {}", sequence,
                 display(from.file()), reason);
    set_message(error, restore_failed_message);
    return false;
  }

  // The new slot: its first version, its manifest, then its file. Anything
  // that fails takes the new slot away again; the old one is never touched.
  SaveVersion first = *version;
  first.sequence = 1;
  first.made_for =
      "branch of version " + std::to_string(sequence) + " of " + filename;
  SlotManifest manifest;
  manifest.next_sequence = 2;
  manifest.active = 1;
  manifest.versions = {first};
  util::AtomicWriteResult written;
  fs::create_directories(to.folder(), ec);
  if (ec) {
    written = {ec.message(), ec};
  }
  else if (written = util::write_file_atomically(to.version_path(1), *bytes);
           written) {
    if (written = to.write_manifest(manifest); written) {
      written = util::write_file_atomically(to.file(), *bytes);
    }
  }
  if (!written) {
    Logger::warn("Branch failed: {}", written.error);
    std::error_code cleanup;
    fs::remove(to.file(), cleanup);
    fs::remove_all(to.folder(), cleanup);
    set_message(error, restore_unwritable_message);
    return false;
  }
  Logger::info("Branched version {} of {} into {}", sequence,
               display(from.file()), display(to.file()));
  return true;
}

bool SaveGameManager::undo_restore(const GameState& state,
                                   const GameContext& context,
                                   const std::string& filename,
                                   std::string* error) {
  const auto manifest = versions(filename);
  if (!manifest || !manifest->previous_active) {
    set_message(error, undo_nothing_message);
    return false;
  }
  return restore_version(state, context, filename, *manifest->previous_active,
                         error);
}

TimedCheckpoints::TimedCheckpoints(fs::path slot_file)
    : slot_file_(std::move(slot_file)) {}

void TimedCheckpoints::frame(GameState& state, const GameContext& context,
                             std::chrono::milliseconds time, bool playing) {
  SaveGameManager::settle_background_saves(state);
  if (job_ && job_->done()) {
    job_->wait();
    static_cast<void>(SaveGameManager::promote(*job_));
    job_.reset();
  }
  if (playing) {
    timer_.played(time);
  }
  timer_.observe(state.time());
  const std::optional<TimedCheckpoint> due = timer_.due();
  if (!due || job_) {
    return;
  }
  job_ = SaveGameManager::start_timed_checkpoint(state, context, *due, nullptr,
                                                 slot_file_);
  timer_.taken();
}

void TimedCheckpoints::finish() {
  if (job_) {
    job_->wait();
    static_cast<void>(SaveGameManager::promote(*job_));
    job_.reset();
  }
  SaveGameManager::finish_background_saves();
}

bool SaveGameManager::delete_save(const std::string& filename,
                                  std::string* error) {
  const fs::path path = get_full_path(filename);
  wait_for_background(path);
  std::error_code file_ec;
  fs::remove(path, file_ec);
  std::error_code folder_ec;
  fs::remove_all(SaveSlot(path).folder(), folder_ec);
  if (file_ec || folder_ec) {
    Logger::warn("Could not delete {}: {}", display(path),
                 (file_ec ? file_ec : folder_ec).message());
    set_message(error, delete_failed_message);
    return false;
  }
  Logger::info("Deleted {} and its versions", display(path));
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

// Every enum json_to_state reads, as the checks expect it.
static_assert(enum_runs(GameMode::FREEMODE, GameMode::QUICKSTART));
static_assert(enum_runs(core::restaurant::StaffEventData::Type::Late,
                        core::restaurant::StaffEventData::Type::ForgotToOpen));
static_assert(enum_runs(core::character::JobRole::CHEF,
                        core::character::JobRole::NONE));
static_assert(enum_runs(TurnOrchestrator::DeferredCall::Kind::MayorInspection,
                        TurnOrchestrator::DeferredCall::Kind::DepotRaid));
static_assert(enum_runs(MayorAwardType::None,
                        MayorAwardType::TransAtlanticTrade));

// MilestoneId runs past magic_enum's reflection range, so it is read by its
// bounds; the unused ids between them index nothing.
static_assert(static_cast<int>(core::events::MilestoneId::LaughingStock) == 1);
static_assert(static_cast<int>(core::events::MilestoneId::ControlsWorld) ==
              core::events::max_milestone_id);
core::events::MilestoneId checked_milestone(const json& value) {
  return checked_enum(value, core::events::MilestoneId::LaughingStock,
                      core::events::MilestoneId::ControlsWorld, "a milestone");
}

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

  // Save format version. A game outside the original ten also needs its
  // places and scenario, which only version 4 carries; a Classic game stays
  // version 3, byte for byte as before.
  j["version"] = SAVE_FORMAT_VERSION;
  if (!state.roster().is_classic()) {
    j["version"] = core::content::expanded_save_version;
    j["scenario"] = core::content::save_scenario_block(
        state.roster(), state.scenario_document());
  }

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
                                    const GameContext& context) {
  // Clear all existing restaurants and reset property states before loading
  state.reset();

  // The places and scenario of a game outside the original ten, before any
  // city is built from them. A damaged block, or more places than this build
  // plays (std::length_error), refuses the save.
  if (const auto it = j.find("scenario"); it != j.end()) {
    std::string problem;
    std::optional<core::content::SavedScenario> saved =
        core::content::read_scenario_block(*it, problem);
    if (!saved) {
      throw std::runtime_error("the scenario is damaged: " + problem);
    }
    state.restore_scenario(std::move(saved->roster),
                           std::move(saved->scenario));
  }
  // A place the game is not played in can't hold anything.
  const auto require_in_roster = [&state](core::CityId city_id,
                                          std::string_view what) {
    if (!is_valid_city_id(city_id) || !state.in_roster(city_id)) {
      throw std::out_of_range(std::string(what) + " is in city " +
                              std::to_string(city_id.value) +
                              ", which this game is not played in");
    }
  };

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
    state.game_mode(checked_enum<GameMode>(j.at("game_mode"), "a game mode"));
  }
  if (j.contains("mission_target")) {
    const auto& mt = j.at("mission_target");
    core::mission::MissionTarget target;
    target.source_name = mt.value("source_name", std::string{});
    for (const auto& milestone : mt.at("milestones")) {
      target.milestones.push_back(
          {save_date(milestone.at("y").get<int>(), milestone.at("m").get<int>(),
                     milestone.at("d").get<int>()),
           checked_milestone(milestone.at("id"))});
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

  // Load players. reset() above emptied every slot, and a game always has
  // all of them (see check_players): the start of a city's day below already
  // walks them without checking.
  {
    const auto& players_json = j.at("players");
    std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
    for (size_t i = 0; i < players.size(); ++i) {
      if (i >= players_json.size() || players_json.at(i).is_null()) {
        throw std::out_of_range("the save has no player " + std::to_string(i));
      }
      players[i] = json_to_player(players_json.at(i), context,
                                  state.customer_taste_profiles());
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
      require_in_roster(city_id, "a city of the save");
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
      if (is_valid_city_id(city_id)) {
        require_in_roster(city_id, "a restaurant");
      }

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
  {
    // A game is always in a city. If somehow there is no valid active city,
    // just pick one.
    core::CityId cid{j.value("active_city", -1)};
    if (is_valid_city_id(cid)) {
      require_in_roster(cid, "the active player");
    }
    else {
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
      data.type = checked_enum<core::restaurant::StaffEventData::Type>(
          ev.at("type"), "a staff event");
      data.job_role = checked_enum<core::character::JobRole>(
          ev.at("job_role"), "a staff event's job");
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
      queued.kind = checked_enum<Kind>(ev.at("kind"), "a queued call");
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
        queued.award.type = checked_enum<MayorAwardType>(ev.at("award_type"),
                                                         "a mayor's award");
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
        auto id = checked_milestone(val.at("id"));
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
        require_in_roster(city_id, "player " + std::to_string(i));
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

int SaveGameManager::remove_stale_temp_files(const std::string& settings_file) {
  const fs::path saves = platform_paths::saves_dir();
  std::vector<fs::path> folders = {
      saves, fs::path(settings_file).parent_path(),
      fs::path(platform_paths::bindings_file_path()).parent_path()};
  // Each slot's versions are written in a folder of their own.
  std::error_code ec;
  fs::directory_iterator it(saves, ec);
  for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    const std::u8string name = it->path().filename().u8string();
    if (it->is_directory(entry_ec) && !entry_ec &&
        is_versions_folder_name(std::string_view(
            reinterpret_cast<const char*>(name.data()), name.size()))) {
      folders.push_back(it->path());
    }
  }
  return remove_stale_temp_files(folders, STALE_TEMP_FILE_AGE);
}

int SaveGameManager::remove_stale_temp_files(
    const std::vector<fs::path>& folders, std::chrono::seconds min_age) {
  std::vector<fs::path> done;
  int removed = 0;
  for (const fs::path& folder : folders) {
    // The settings and bindings files usually share a folder; "saves/" and
    // "saves" are the same one.
    fs::path where =
        (folder.empty() ? fs::path(".") : folder).lexically_normal();
    if (!where.has_filename() && where.has_relative_path()) {
      where = where.parent_path();
    }
    if (std::find(done.begin(), done.end(), where) != done.end()) {
      continue;
    }
    done.push_back(where);

    const util::StaleTempFiles result =
        util::remove_stale_temp_files(where, min_age);
    for (const fs::path& file : result.removed) {
      Logger::info("Removed {}, left by a write that never finished",
                   display(file));
      ++removed;
    }
    for (const auto& [file, reason] : result.failed) {
      Logger::warn("Could not clear leftover temporary files at {}: {}",
                   display(file), reason);
    }
  }
  return removed;
}

StateFingerprint SaveGameManager::fingerprint(const GameState& state,
                                              const GameContext& context) {
  json document = state_to_json(state, context);
  document.erase("saved_at");

  // nlohmann::json orders objects by key, so the same state always dumps to the
  // same bytes.
  return StateFingerprint{.hash = fingerprint_hash(document.dump()),
                          .document = std::move(document)};
}

} // namespace save
