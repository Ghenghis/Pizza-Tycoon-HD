/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/Session.h"

#include <stdexcept>

namespace save {

namespace {

constexpr std::string_view session_format = "plegacy-session";
constexpr std::uint64_t max_session_size = 64 * 1024;

std::string text_member(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().size() > 64) {
    throw std::runtime_error(std::string("the session record's ") + key +
                             " is missing or not a time");
  }
  return it->get<std::string>();
}

} // namespace

nlohmann::json session_to_json(const SessionRecord& record) {
  return {{"format", session_format},
          {"format_version", 1},
          {"started_at", record.started_at},
          {"clean_exit", record.clean_exit},
          {"last_manual_save_at", record.last_manual_save_at}};
}

SessionRecord session_from_json(const nlohmann::json& j) {
  if (!j.is_object() || j.value("format", "") != session_format ||
      j.value("format_version", 0) != 1) {
    throw std::runtime_error("not a session record this build reads");
  }
  const auto clean = j.find("clean_exit");
  if (clean == j.end() || !clean->is_boolean()) {
    throw std::runtime_error("the session record has no clean_exit");
  }
  return {.started_at = text_member(j, "started_at"),
          .clean_exit = clean->get<bool>(),
          .last_manual_save_at = text_member(j, "last_manual_save_at")};
}

std::optional<SessionRecord> read_session(const std::filesystem::path& file,
                                          std::string& problem) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    problem = ec ? ec.message() : "";
    return std::nullopt;
  }
  const FileRead read = read_whole_file(file, max_session_size);
  if (!read.bytes) {
    problem = read.problem;
    return std::nullopt;
  }
  try {
    return session_from_json(nlohmann::json::parse(*read.bytes));
  }
  catch (const std::exception& e) {
    problem = e.what();
    return std::nullopt;
  }
}

util::AtomicWriteResult write_session(const std::filesystem::path& file,
                                      const SessionRecord& record) {
  return util::write_file_atomically(file,
                                     session_to_json(record).dump(2) + "\n");
}

std::optional<ResumeCandidate>
choose_resume(const std::optional<SessionRecord>& previous,
              const std::optional<SlotManifest>& timed_checkpoints) {
  if (!previous || previous->clean_exit || previous->started_at.empty() ||
      !timed_checkpoints) {
    return std::nullopt;
  }
  // The times are all UTC in one fixed format, so they sort as text.
  const SaveVersion* newest = nullptr;
  for (const SaveVersion& v : timed_checkpoints->versions) {
    if (v.status != VersionStatus::Working ||
        v.written_at < previous->started_at ||
        (!previous->last_manual_save_at.empty() &&
         v.written_at <= previous->last_manual_save_at)) {
      continue;
    }
    if (newest == nullptr || v.sequence > newest->sequence) {
      newest = &v;
    }
  }
  if (newest == nullptr) {
    return std::nullopt;
  }
  return ResumeCandidate{.sequence = newest->sequence,
                         .written_at = newest->written_at,
                         .game_date = newest->game_date};
}

} // namespace save
