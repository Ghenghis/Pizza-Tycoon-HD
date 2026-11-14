/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/AtomicFile.h"

namespace music {

// Parses JSON, stopping as soon as the document nests deeper than max_depth
// or holds more than max_values values. nullopt with the reason otherwise.
[[nodiscard]] std::optional<nlohmann::json> parse_json(std::string_view text,
                                                       int max_depth,
                                                       std::uint64_t max_values,
                                                       std::string* error);

// The whole file, when it is at most max_bytes. nullopt when it is not
// there (error left empty) or cannot be read (error says why).
[[nodiscard]] std::optional<std::string>
read_text_file(const std::filesystem::path& file, std::uint64_t max_bytes,
               std::string* error);

// A JSON document kept so that a crash, a power cut or a damaged file loses
// at most the last change. A save writes a candidate file beside it
// (through util::AtomicFile), reads it back and checks it; only then does the
// current file become backup 1 (1 becomes 2, 2 becomes 3) and the candidate
// take its place. A damaged current file is moved aside, not kept as a
// backup. Every copy carries the SHA-256 of its data. Loading takes the
// current file when it checks out, otherwise the candidate, otherwise the
// newest good backup.
class RevisionedDocument {
public:
  static constexpr int BACKUPS = 3;
  static constexpr std::uint64_t MAX_BYTES = std::uint64_t{32} << 20;

  // format names the document ("pthd-music-settings"); schema is the
  // version of its data this game reads and writes.
  RevisionedDocument(std::filesystem::path file, std::string format,
                     int schema);

  struct Loaded {
    nlohmann::json data;
    std::string source; // "current", "candidate", "backup 1" ...
  };
  // nullopt when there is no copy at all (a first start), or when none
  // checks out; problems then lists why each one was refused.
  [[nodiscard]] std::optional<Loaded>
  load(std::vector<std::string>* problems = nullptr) const;

  [[nodiscard]] util::AtomicWriteResult save(const nlohmann::json& data);

  [[nodiscard]] const std::filesystem::path& file() const { return file_; }
  [[nodiscard]] std::filesystem::path candidate_path() const;
  [[nodiscard]] std::filesystem::path backup_path(int n) const; // 1 to 3
  [[nodiscard]] std::filesystem::path damaged_path() const;

private:
  // The data of the copy at path, when it checks out; why not otherwise.
  [[nodiscard]] std::optional<nlohmann::json>
  read_copy(const std::filesystem::path& path, std::string* why) const;

  std::filesystem::path file_;
  std::string format_;
  int schema_;
};

} // namespace music
