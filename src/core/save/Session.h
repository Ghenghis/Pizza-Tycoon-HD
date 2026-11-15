/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the game remembers of its last run, to offer resuming after one that
// did not end cleanly: when it began, whether it ended cleanly, and when the
// player last saved by hand. Kept in "session.json" in the saves folder.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/save/SaveVersions.h"
#include "core/util/AtomicFile.h"

namespace save {

constexpr std::string_view session_filename = "session.json";

struct SessionRecord {
  std::string started_at; // UTC, as SaveVersion::written_at
  bool clean_exit = false;
  std::string last_manual_save_at; // UTC, or empty if there was none
};

[[nodiscard]] nlohmann::json session_to_json(const SessionRecord& record);
// Throws std::runtime_error for anything else.
[[nodiscard]] SessionRecord session_from_json(const nlohmann::json& j);

// Nothing if there is no record or it can't be read; problem says which.
[[nodiscard]] std::optional<SessionRecord>
read_session(const std::filesystem::path& file, std::string& problem);
[[nodiscard]] util::AtomicWriteResult
write_session(const std::filesystem::path& file, const SessionRecord& record);

// A version the start-up may offer to resume.
struct ResumeCandidate {
  std::uint64_t sequence = 0;
  std::string written_at;
  std::string game_date;
};

// The decision, from what is on disk. After a run that did not end cleanly,
// the newest working timed checkpoint that run made after its last manual
// save. Nothing after a clean exit, without a record of the run, or when the
// player saved by hand after the last checkpoint. Checkpoints made before a
// new game or a load hold the game that was replaced, not the one last
// played, so only timed ones count.
[[nodiscard]] std::optional<ResumeCandidate>
choose_resume(const std::optional<SessionRecord>& previous,
              const std::optional<SlotManifest>& timed_checkpoints);

} // namespace save
