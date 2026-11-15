/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/SaveVersions.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <zlib.h>
// zlib.h defines FAR as a macro for legacy DOS far-pointer compat, which
// conflicts with our ZoomLevel::FAR
#undef FAR

#include "core/save/SaveGameManager.h"

namespace save {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::string_view manifest_format = "plegacy-save-versions";
constexpr std::string_view manifest_file_name = "manifest.json";
constexpr std::string_view versions_suffix = ".versions";
constexpr std::string_view version_prefix = "v";
constexpr std::size_t version_digits = 6;
constexpr std::string_view checksum_prefix = "crc32:";

// A manifest lists a handful of versions; these bounds only stop a damaged
// or hostile one from taking up memory.
constexpr std::uint64_t max_manifest_size = 1024 * 1024;
constexpr int max_manifest_depth = 8;
constexpr std::uint64_t max_manifest_values = 100'000;
constexpr std::size_t max_listed_versions = 64;
constexpr std::size_t max_retired_versions = 256;
constexpr std::size_t max_text_length = 4096;
constexpr std::size_t fingerprint_length = 16;

// A manifest from a newer build than this one.
class NewerManifest : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void invalid(const std::string& what) {
  throw std::runtime_error(what);
}

bool is_lower_hex(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string hex(std::uint64_t value, std::size_t digits) {
  constexpr std::string_view symbols = "0123456789abcdef";
  std::string out(digits, '0');
  for (std::size_t i = digits; i-- > 0;) {
    out[i] = symbols[value & 0xF];
    value >>= 4;
  }
  return out;
}

const json& member(const json& object, std::string_view key,
                   std::string_view where) {
  const auto it = object.find(key);
  if (it == object.end()) {
    invalid(std::string(where) + " has no " + std::string(key));
  }
  return *it;
}

std::uint64_t unsigned_value(const json& value, std::string_view what) {
  if (!value.is_number_unsigned() &&
      !(value.is_number_integer() && value.get<std::int64_t>() >= 0)) {
    invalid(std::string(what) + " is not a whole number of zero or more");
  }
  return value.get<std::uint64_t>();
}

std::string text_value(const json& value, std::string_view what) {
  if (!value.is_string()) {
    invalid(std::string(what) + " is not text");
  }
  std::string text = value.get<std::string>();
  if (text.size() > max_text_length) {
    invalid(std::string(what) + " is longer than any the game writes");
  }
  return text;
}

VersionStatus status_from(std::string_view text) {
  for (const VersionStatus status :
       {VersionStatus::Working, VersionStatus::Candidate}) {
    if (text == to_string(status)) {
      return status;
    }
  }
  invalid("a version has the status \"" + std::string(text) + "\"");
}

VersionLevel level_from(std::string_view text) {
  for (const VersionLevel level :
       {VersionLevel::Written, VersionLevel::IntegrityChecked,
        VersionLevel::LoadVerified}) {
    if (text == to_string(level)) {
      return level;
    }
  }
  invalid("a version has the level \"" + std::string(text) + "\"");
}

SaveVersion version_from_json(const json& j, std::uint64_t next_sequence) {
  if (!j.is_object()) {
    invalid("a version is not an object");
  }
  SaveVersion v;
  v.sequence = unsigned_value(member(j, "sequence", "a version"), "sequence");
  const std::string where = "version " + std::to_string(v.sequence);
  if (v.sequence == 0 || v.sequence >= next_sequence) {
    invalid(where + " is outside the slot's sequence");
  }
  v.status = status_from(text_value(member(j, "status", where), "status"));
  v.level = level_from(text_value(member(j, "level", where), "level"));
  if (v.status == VersionStatus::Working &&
      v.level != VersionLevel::LoadVerified) {
    invalid(where + " is working without being load-verified");
  }
  v.reason = text_value(member(j, "reason", where), "reason");
  v.made_for = text_value(member(j, "made_for", where), "made_for");
  v.written_at = text_value(member(j, "written_at", where), "written_at");
  v.size = unsigned_value(member(j, "size", where), "size");
  if (v.size > MAX_SAVE_FILE_SIZE) {
    invalid(where + " is larger than any save");
  }
  v.checksum = text_value(member(j, "checksum", where), "checksum");
  if (v.checksum.size() != checksum_prefix.size() + 8 ||
      !v.checksum.starts_with(checksum_prefix) ||
      !is_lower_hex(
          std::string_view(v.checksum).substr(checksum_prefix.size()))) {
    invalid(where + " has the checksum \"" + v.checksum + "\"");
  }
  v.fingerprint = text_value(member(j, "fingerprint", where), "fingerprint");
  if (v.fingerprint.size() != fingerprint_length ||
      !is_lower_hex(v.fingerprint)) {
    invalid(where + " has the fingerprint \"" + v.fingerprint + "\"");
  }
  v.game_date = text_value(member(j, "game_date", where), "game_date");
  return v;
}

json version_to_json(const SaveVersion& v) {
  return {
      {"sequence", v.sequence},
      {"status", to_string(v.status)},
      {"level", to_string(v.level)},
      {"reason", v.reason},
      {"made_for", v.made_for},
      {"written_at", v.written_at},
      {"size", v.size},
      {"checksum", v.checksum},
      {"fingerprint", v.fingerprint},
      {"game_date", v.game_date},
  };
}

// Parses JSON, stopping as soon as it nests too deep or holds too many
// values, before they take up the memory.
json parse_manifest(std::string_view text) {
  std::uint64_t values = 0;
  const json::parser_callback_t check =
      [&values](int depth, json::parse_event_t event, json& /*parsed*/) {
        if (depth > max_manifest_depth) {
          invalid("the manifest nests deeper than any the game writes");
        }
        if (event == json::parse_event_t::object_start ||
            event == json::parse_event_t::array_start ||
            event == json::parse_event_t::value) {
          if (++values > max_manifest_values) {
            invalid("the manifest holds more than any the game writes");
          }
        }
        return true;
      };
  return json::parse(text.data(), text.data() + text.size(), check);
}

bool newest_first(const SaveVersion& a, const SaveVersion& b) {
  return a.sequence > b.sequence;
}

} // namespace

const SaveVersion* SlotManifest::find(std::uint64_t sequence) const {
  const auto it = std::ranges::find(versions, sequence, &SaveVersion::sequence);
  return it == versions.end() ? nullptr : &*it;
}

SaveVersion* SlotManifest::find(std::uint64_t sequence) {
  const auto it = std::ranges::find(versions, sequence, &SaveVersion::sequence);
  return it == versions.end() ? nullptr : &*it;
}

const SaveVersion* SlotManifest::newest_working() const {
  const SaveVersion* newest = nullptr;
  for (const SaveVersion& v : versions) {
    if (v.status == VersionStatus::Working &&
        (newest == nullptr || v.sequence > newest->sequence)) {
      newest = &v;
    }
  }
  return newest;
}

std::string checksum_of(std::string_view bytes) {
  uLong crc = crc32(0L, Z_NULL, 0);
  const auto* data = reinterpret_cast<const Bytef*>(bytes.data());
  std::size_t left = bytes.size();
  // zlib takes lengths as uInt, which may be narrower than size_t.
  while (left > 0) {
    const auto chunk = static_cast<uInt>(std::min<std::size_t>(left, UINT_MAX));
    crc = crc32(crc, data, chunk);
    data += chunk;
    left -= chunk;
  }
  return std::string(checksum_prefix) + hex(crc & 0xFFFFFFFFUL, 8);
}

FileDigest digest_of(std::string_view bytes) {
  return {.size = bytes.size(), .checksum = checksum_of(bytes)};
}

FileRead read_whole_file(const fs::path& path, std::uint64_t max_size) {
  FileRead read;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    read.problem = "cannot open it";
    return read;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  in.seekg(0, std::ios::beg);
  if (!in.good() || end < 0) {
    read.problem = "cannot read it";
    return read;
  }
  if (static_cast<std::uint64_t>(end) > max_size) {
    read.problem =
        "it is " + std::to_string(end) + " bytes, more than it can be";
    read.too_large = true;
    return read;
  }
  std::string bytes(static_cast<std::size_t>(end), '\0');
  if (!bytes.empty() &&
      !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    read.problem = "cannot read it";
    return read;
  }
  read.bytes = std::move(bytes);
  return read;
}

std::string utc_now() {
  using namespace std::chrono;
  const auto now = floor<seconds>(system_clock::now());
  const auto day = floor<days>(now);
  const year_month_day date{day};
  const hh_mm_ss time{now - day};
  std::array<char, 32> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
      static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
      static_cast<unsigned>(date.day()), static_cast<int>(time.hours().count()),
      static_cast<int>(time.minutes().count()),
      static_cast<int>(time.seconds().count()));
  return std::string(text.data(),
                     static_cast<std::size_t>(std::max(length, 0)));
}

std::string_view to_string(VersionLevel level) {
  switch (level) {
  case VersionLevel::Written:
    return "written";
  case VersionLevel::IntegrityChecked:
    return "integrity-checked";
  case VersionLevel::LoadVerified:
    return "load-verified";
  }
  return "written";
}

std::string_view to_string(VersionStatus status) {
  switch (status) {
  case VersionStatus::Working:
    return "working";
  case VersionStatus::Candidate:
    return "candidate";
  }
  return "candidate";
}

json manifest_to_json(const SlotManifest& manifest,
                      std::string_view slot_name) {
  json versions = json::array();
  for (const SaveVersion& v : manifest.versions) {
    versions.push_back(version_to_json(v));
  }
  return {
      {"format", manifest_format},
      {"format_version", SAVE_VERSIONS_FORMAT},
      {"slot", slot_name},
      {"next_sequence", manifest.next_sequence},
      {"active", manifest.active ? json(*manifest.active) : json(nullptr)},
      {"previous_active", manifest.previous_active
                              ? json(*manifest.previous_active)
                              : json(nullptr)},
      {"versions", std::move(versions)},
      {"retired", manifest.retired},
  };
}

SlotManifest manifest_from_json(const json& j) {
  if (!j.is_object()) {
    invalid("the manifest is not a JSON object");
  }
  if (text_value(member(j, "format", "the manifest"), "format") !=
      manifest_format) {
    invalid("the manifest is not a save versions manifest");
  }
  const json& format_version = member(j, "format_version", "the manifest");
  if (!format_version.is_number_integer()) {
    invalid("the manifest's format_version is not a number");
  }
  if (format_version.get<std::int64_t>() > SAVE_VERSIONS_FORMAT) {
    throw NewerManifest("the manifest is format " +
                        std::to_string(format_version.get<std::int64_t>()) +
                        "; this build reads up to " +
                        std::to_string(SAVE_VERSIONS_FORMAT));
  }
  if (format_version.get<std::int64_t>() < 1) {
    invalid("the manifest has format version " +
            std::to_string(format_version.get<std::int64_t>()));
  }

  SlotManifest manifest;
  manifest.next_sequence = unsigned_value(
      member(j, "next_sequence", "the manifest"), "next_sequence");
  if (manifest.next_sequence == 0) {
    invalid("the manifest's next_sequence is 0");
  }

  const json& versions = member(j, "versions", "the manifest");
  if (!versions.is_array() || versions.size() > max_listed_versions) {
    invalid("the manifest's versions are not a short list");
  }
  std::unordered_set<std::uint64_t> listed;
  for (const json& entry : versions) {
    SaveVersion v = version_from_json(entry, manifest.next_sequence);
    if (!listed.insert(v.sequence).second) {
      invalid("version " + std::to_string(v.sequence) + " is listed twice");
    }
    manifest.versions.push_back(std::move(v));
  }
  std::ranges::sort(manifest.versions, newest_first);

  const json& active = member(j, "active", "the manifest");
  if (!active.is_null()) {
    const std::uint64_t sequence = unsigned_value(active, "active");
    const SaveVersion* v = manifest.find(sequence);
    if (v == nullptr || v->status != VersionStatus::Working) {
      invalid("the active version " + std::to_string(sequence) +
              " is not a working version");
    }
    manifest.active = sequence;
  }

  // Optional: manifests from before undo had none. One that no longer
  // names a working version has nothing left to undo to.
  if (const auto it = j.find("previous_active");
      it != j.end() && !it->is_null()) {
    const std::uint64_t sequence = unsigned_value(*it, "previous_active");
    const SaveVersion* v = manifest.find(sequence);
    if (v != nullptr && v->status == VersionStatus::Working) {
      manifest.previous_active = sequence;
    }
  }

  const json& retired = member(j, "retired", "the manifest");
  if (!retired.is_array() || retired.size() > max_retired_versions) {
    invalid("the manifest's retired versions are not a short list");
  }
  for (const json& entry : retired) {
    const std::uint64_t sequence = unsigned_value(entry, "a retired version");
    if (sequence == 0 || sequence >= manifest.next_sequence ||
        listed.contains(sequence)) {
      invalid("retired version " + std::to_string(sequence) +
              " is not one the slot dropped");
    }
    manifest.retired.push_back(sequence);
  }
  return manifest;
}

void apply_retention(SlotManifest& manifest) {
  std::ranges::sort(manifest.versions, newest_first);
  int working_kept = 0;
  bool working_newer = false;
  bool candidate_kept = false;
  std::vector<SaveVersion> kept;
  for (SaveVersion& v : manifest.versions) {
    const bool active = manifest.active && *manifest.active == v.sequence;
    bool keep = false;
    if (v.status == VersionStatus::Working) {
      keep = active || working_kept < WORKING_VERSIONS_KEPT;
      working_kept += keep ? 1 : 0;
      working_newer = true;
    }
    else {
      keep = !candidate_kept && !working_newer;
      candidate_kept = candidate_kept || keep;
    }
    if (keep) {
      kept.push_back(std::move(v));
    }
    else if (std::ranges::find(manifest.retired, v.sequence) ==
             manifest.retired.end()) {
      manifest.retired.push_back(v.sequence);
    }
  }
  manifest.versions = std::move(kept);
}

SaveSlot::SaveSlot(fs::path slot_file, std::string extension)
    : file_(std::move(slot_file)), extension_(std::move(extension)) {}

fs::path SaveSlot::folder() const {
  fs::path name = file_.filename();
  name += versions_suffix;
  return file_.parent_path() / name;
}

fs::path SaveSlot::manifest_path() const {
  return folder() / manifest_file_name;
}

fs::path SaveSlot::version_path(std::uint64_t sequence) const {
  std::string digits = std::to_string(sequence);
  if (digits.size() < version_digits) {
    digits.insert(0, version_digits - digits.size(), '0');
  }
  return folder() / (std::string(version_prefix) + digits +
                     extension_);
}

bool SaveSlot::has_versions() const {
  std::error_code ec;
  return fs::is_directory(folder(), ec);
}

SaveSlot::ManifestRead SaveSlot::read_manifest() const {
  ManifestRead read;
  const fs::path path = manifest_path();
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  if (ec) {
    read.problem = "cannot look for the manifest: " + ec.message();
    return read;
  }
  if (!exists) {
    return read;
  }

  const FileRead text = read_whole_file(path, max_manifest_size);
  if (!text.bytes) {
    read.problem = "the manifest: " + text.problem;
    return read;
  }
  try {
    read.manifest = manifest_from_json(parse_manifest(*text.bytes));
  }
  catch (const NewerManifest& e) {
    read.problem = e.what();
    read.newer = true;
  }
  catch (const std::exception& e) {
    read.problem = e.what();
  }
  return read;
}

util::AtomicWriteResult
SaveSlot::write_manifest(const SlotManifest& manifest) const {
  const std::u8string name = file_.filename().u8string();
  const std::string text =
      manifest_to_json(manifest, std::string(name.begin(), name.end()))
          .dump(2) +
      "\n";
  return util::write_file_atomically(manifest_path(), text);
}

std::vector<std::uint64_t> SaveSlot::version_files() const {
  std::vector<std::uint64_t> found;
  std::error_code ec;
  fs::directory_iterator it(folder(), ec);
  for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    const std::u8string name = it->path().filename().u8string();
    if (const auto sequence = version_sequence(
            std::string_view(reinterpret_cast<const char*>(name.data()),
                             name.size()),
            extension_)) {
      found.push_back(*sequence);
    }
  }
  std::ranges::sort(found, std::greater<>{});
  return found;
}

std::optional<std::uint64_t> version_sequence(std::string_view file_name,
                                              std::string_view version_extension) {
  if (!file_name.starts_with(version_prefix) ||
      !file_name.ends_with(version_extension)) {
    return std::nullopt;
  }
  const std::string_view digits = file_name.substr(
      version_prefix.size(),
      file_name.size() - version_prefix.size() - version_extension.size());
  if (digits.size() < version_digits) {
    return std::nullopt;
  }
  std::uint64_t sequence = 0;
  const auto [end, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), sequence);
  if (ec != std::errc{} || end != digits.data() + digits.size() ||
      sequence == 0) {
    return std::nullopt;
  }
  // One name per version: no extra leading zeros past the padding.
  if (digits.size() > version_digits && digits.front() == '0') {
    return std::nullopt;
  }
  return sequence;
}

bool is_versions_folder_name(std::string_view name) {
  constexpr std::string_view tail = ".plsave.versions";
  return name.size() > tail.size() && name.ends_with(tail);
}

} // namespace save
