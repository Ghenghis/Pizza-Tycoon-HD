/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The versions a save slot keeps: its last three verified working saves, and
// the newest save that did not pass its checks, the candidate. They live in a
// folder beside the slot's file, "<slot>.versions", with a manifest that says
// which is which. docs/studio/notes/save-versions.md describes the manifest.
//
// This is the file side only: names, the manifest, checksums and which
// versions to keep. SaveGameManager writes, checks and loads the saves.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/util/AtomicFile.h"

namespace save {

// How many verified working versions a slot keeps.
constexpr int WORKING_VERSIONS_KEPT = 3;

// The manifest format this build writes and reads.
constexpr int SAVE_VERSIONS_FORMAT = 1;

// How far a version got through the checks.
enum class VersionLevel {
  Written,          // on disk, not checked yet, or failed the first check
  IntegrityChecked, // its bytes and header are what was written
  LoadVerified,     // it loads, apart from the game, to the game that was saved
};

// Whether a version may be used: a working version passed every check; a
// candidate did not, and says why.
enum class VersionStatus { Working, Candidate };

struct SaveVersion {
  std::uint64_t sequence = 0; // one more than the slot's previous version
  VersionStatus status = VersionStatus::Candidate;
  VersionLevel level = VersionLevel::Written;
  std::string reason;      // why a candidate is not working; empty otherwise
  std::string made_for;    // "save", "autosave", "checkpoint before ..."
  std::string written_at;  // UTC, as 2026-11-14T09:30:12Z
  std::uint64_t size = 0;  // of the file, in bytes
  std::string checksum;    // of the file: "crc32:" and 8 hex digits
  std::string fingerprint; // SaveGameManager::fingerprint of the saved game
  std::string game_date;   // the game's date, as the load menu shows it
};

struct SlotManifest {
  std::uint64_t next_sequence = 1;
  // The working version the slot's file holds and loading starts from.
  std::optional<std::uint64_t> active;
  // The version that was active before the last restore, for undoing it.
  std::optional<std::uint64_t> previous_active;
  std::vector<SaveVersion> versions; // newest first
  // Versions dropped from the list whose files may still have to go.
  std::vector<std::uint64_t> retired;

  [[nodiscard]] const SaveVersion* find(std::uint64_t sequence) const;
  [[nodiscard]] SaveVersion* find(std::uint64_t sequence);
  // The newest working version, if there is one.
  [[nodiscard]] const SaveVersion* newest_working() const;
};

// Size and checksum of a file's bytes.
struct FileDigest {
  std::uint64_t size = 0;
  std::string checksum;

  friend bool operator==(const FileDigest&, const FileDigest&) = default;
};

[[nodiscard]] std::string checksum_of(std::string_view bytes);
[[nodiscard]] FileDigest digest_of(std::string_view bytes);

struct FileRead {
  std::optional<std::string> bytes; // nothing if the file could not be read
  std::string problem;              // why not
  bool too_large = false;           // because it is larger than allowed
};

// Reads a whole file of at most max_size bytes.
[[nodiscard]] FileRead read_whole_file(const std::filesystem::path& path,
                                       std::uint64_t max_size);

// The current time in UTC, as written_at holds it.
[[nodiscard]] std::string utc_now();

[[nodiscard]] std::string_view to_string(VersionLevel level);
[[nodiscard]] std::string_view to_string(VersionStatus status);

// The manifest as written to disk, and back. manifest_from_json throws
// std::runtime_error, saying what is wrong, for anything but a manifest this
// build wrote or could have written.
[[nodiscard]] nlohmann::json manifest_to_json(const SlotManifest& manifest,
                                              std::string_view slot_name);
[[nodiscard]] SlotManifest manifest_from_json(const nlohmann::json& j);

// Drops what a slot no longer keeps, moving it to retired: working versions
// past the newest WORKING_VERSIONS_KEPT, and every candidate but the newest
// one, which stays only while no working version is newer. The active
// version always stays. So a save that fails its checks never costs a
// working version.
void apply_retention(SlotManifest& manifest);

// A slot's file and the folder of versions beside it.
class SaveSlot {
public:
  explicit SaveSlot(std::filesystem::path slot_file);

  [[nodiscard]] const std::filesystem::path& file() const { return file_; }
  [[nodiscard]] std::filesystem::path folder() const;
  [[nodiscard]] std::filesystem::path manifest_path() const;
  [[nodiscard]] std::filesystem::path
  version_path(std::uint64_t sequence) const;

  // Whether the slot has a folder of versions at all.
  [[nodiscard]] bool has_versions() const;

  struct ManifestRead {
    // Nothing, with problem empty, if the slot has no manifest yet.
    std::optional<SlotManifest> manifest;
    std::string problem; // why the manifest there could not be used
    // A manifest from a newer build. Leave it, and the versions, alone.
    bool newer = false;
  };
  [[nodiscard]] ManifestRead read_manifest() const;

  // Writes the manifest in one step, through util::AtomicFile.
  [[nodiscard]] util::AtomicWriteResult
  write_manifest(const SlotManifest& manifest) const;

  // The sequence numbers of the version files in the folder, whatever the
  // manifest says, newest first.
  [[nodiscard]] std::vector<std::uint64_t> version_files() const;

private:
  std::filesystem::path file_;
};

// The sequence number in a version file's name, "v000013.plsave".
[[nodiscard]] std::optional<std::uint64_t>
version_sequence(std::string_view file_name);

// Whether a folder in the saves folder holds a slot's versions.
[[nodiscard]] bool is_versions_folder_name(std::string_view name);

} // namespace save
