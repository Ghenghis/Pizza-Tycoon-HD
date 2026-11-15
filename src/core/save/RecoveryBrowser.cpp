/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/RecoveryBrowser.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

#include "engine/FormatCompat.h"

namespace save {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view extension = ".plsave";

std::string display_name(std::string_view file) {
  if (file.ends_with(extension)) {
    file.remove_suffix(extension.size());
  }
  return std::string(file);
}

// "2026-11-14 09:30", from "2026-11-14T09:30:12Z".
std::string short_time(const std::string& utc) {
  if (utc.size() >= 16) {
    return utc.substr(0, 10) + " " + utc.substr(11, 5);
  }
  return utc;
}

// What made a version, in a word or two for the list.
std::string short_reason(const std::string& made_for) {
  struct Short {
    std::string_view prefix;
    std::string_view label;
  };
  constexpr Short shorts[] = {
      {"checkpoint before a new game", "before new game"},
      {"checkpoint before loading", "before load"},
      {"checkpoint after", "timed"},
      {"checkpoint at a new day", "new day"},
      {"checkpoint at a new week", "new week"},
      {"branch of", "branch"},
  };
  for (const Short& s : shorts) {
    if (made_for.starts_with(s.prefix)) {
      return std::string(s.label);
    }
  }
  return made_for;
}

std::string_view what_level_means(VersionLevel level) {
  switch (level) {
  case VersionLevel::Written:
    return "It was written, but did not pass a check.";
  case VersionLevel::IntegrityChecked:
    return "Its bytes and header are as written, but it was not shown to "
           "load to the game that was saved.";
  case VersionLevel::LoadVerified:
    return "Load-verified: built apart from the game, it was the very game "
           "that was saved.";
  }
  return "";
}

bool is_checkpoint_slot(const RecoverySlot& slot) { return slot.checkpoint; }

RecoveryRow slot_row(const RecoverySlot& slot) {
  RecoveryRow row;
  row.kind = RecoveryRow::Kind::Slot;
  row.slot = slot.name;
  row.checkpoint = slot.checkpoint;
  const std::string name = display_name(slot.name);
  std::string tag = slot.checkpoint ? "  (made by the game)" : "";

  if (!slot.problem.empty()) {
    row.damaged = true;
    row.text = name + tag + " - its versions are damaged";
    row.detail = "The list of " + name + "'s versions can't be read: " +
                 slot.problem +
                 ". Nothing in it is deleted. Loading it tries its file and "
                 "every version found, newest first.";
    row.can_load = slot.has_file;
    return row;
  }
  if (!slot.versions) {
    row.text = name + tag + " - one file, no versions";
    row.detail = name +
                 " is a single save file, from before saves kept versions or "
                 "written by hand. Loading it checks it whole first.";
    row.can_load = slot.has_file;
    return row;
  }

  const SlotManifest& m = *slot.versions;
  const auto working = static_cast<std::size_t>(std::ranges::count(
      m.versions, VersionStatus::Working, &SaveVersion::status));
  row.text = compat::format("{}{} - {} working, {} other", name, tag, working,
                            m.versions.size() - working);
  row.detail = compat::format(
      "{} keeps its last {} verified working versions and the newest one that "
      "did not pass its checks. Loading {} gives the one marked *.",
      name, WORKING_VERSIONS_KEPT, name);
  if (m.previous_active) {
    if (const SaveVersion* before = m.find(*m.previous_active);
        before != nullptr && before->status == VersionStatus::Working) {
      row.can_undo = true;
      row.detail += compat::format(
          " Undo last restore makes version {} the one it loads again.",
          before->sequence);
    }
  }
  return row;
}

RecoveryRow version_row(const RecoverySlot& slot, const SlotManifest& m,
                        const SaveVersion& v) {
  RecoveryRow row;
  row.kind = RecoveryRow::Kind::Version;
  row.slot = slot.name;
  row.sequence = v.sequence;
  row.checkpoint = slot.checkpoint;
  row.active = m.active == v.sequence;
  const bool working = v.status == VersionStatus::Working;
  row.damaged = !working && v.reason != awaiting_promotion;

  std::string state =
      working ? std::string(to_string(v.level))
              : (v.reason == awaiting_promotion ? std::string("being saved")
                                                : "damaged (" +
                                                      std::string(to_string(
                                                          v.level)) +
                                                      ")");
  row.text = compat::format("    #{:<4} {:<26} {}  {}  {}{}", v.sequence, state,
                            short_time(v.written_at), v.game_date,
                            short_reason(v.made_for), row.active ? "  *" : "");

  const std::string name = display_name(slot.name);
  row.detail = compat::format("Version {} of {}, saved {} UTC, game day {}, "
                              "made by: {}. ",
                              v.sequence, name, short_time(v.written_at),
                              v.game_date.empty() ? "unknown" : v.game_date,
                              v.made_for);
  row.detail += what_level_means(v.level);
  if (!working) {
    row.detail += v.reason == awaiting_promotion
                      ? " It is still being saved."
                      : " Why it can't be used: " + v.reason + ".";
  }
  else if (row.active) {
    row.detail += " Loading " + name + " gives this version.";
  }
  row.can_load = working;
  row.can_branch = working;
  return row;
}

} // namespace

std::vector<RecoveryRow> recovery_rows(const std::vector<RecoverySlot>& slots) {
  std::vector<const RecoverySlot*> order;
  order.reserve(slots.size());
  for (const RecoverySlot& slot : slots) {
    order.push_back(&slot);
  }
  std::ranges::stable_sort(order, [](const RecoverySlot* a,
                                     const RecoverySlot* b) {
    if (is_checkpoint_slot(*a) != is_checkpoint_slot(*b)) {
      return is_checkpoint_slot(*a);
    }
    return a->name < b->name;
  });

  std::vector<RecoveryRow> rows;
  for (const RecoverySlot* slot : order) {
    rows.push_back(slot_row(*slot));
    if (slot->versions) {
      std::vector<const SaveVersion*> versions;
      for (const SaveVersion& v : slot->versions->versions) {
        versions.push_back(&v);
      }
      std::ranges::sort(versions, std::greater<>{}, &SaveVersion::sequence);
      for (const SaveVersion* v : versions) {
        rows.push_back(version_row(*slot, *slot->versions, *v));
      }
    }
  }
  return rows;
}

std::string branch_name(std::string_view slot, std::uint64_t sequence,
                        const std::vector<RecoverySlot>& slots) {
  const auto taken = [&slots](const std::string& name) {
    return std::ranges::any_of(slots, [&name](const RecoverySlot& s) {
      return std::ranges::equal(s.name, name, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
    });
  };
  const std::string base =
      display_name(slot) + "-" + std::to_string(sequence);
  std::string name = base + std::string(extension);
  for (char suffix = 'b'; taken(name) && suffix <= 'z'; ++suffix) {
    name = base + suffix + std::string(extension);
  }
  for (int n = 2; taken(name); ++n) {
    name = base + "-" + std::to_string(n) + std::string(extension);
  }
  return name;
}

RecoveryBrowser::RecoveryBrowser(std::size_t rows_per_page, fs::path folder)
    : per_page_(std::max<std::size_t>(rows_per_page, 1)),
      folder_(std::move(folder)) {
  refresh();
}

void RecoveryBrowser::refresh() {
  std::optional<std::pair<std::string, std::optional<std::uint64_t>>> keep;
  if (const RecoveryRow* row = chosen()) {
    keep.emplace(row->slot, row->sequence);
  }
  slots_ = SaveGameManager::recovery_slots(folder_);
  rows_ = recovery_rows(slots_);
  chosen_.reset();
  if (keep) {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].slot == keep->first && rows_[i].sequence == keep->second) {
        chosen_ = i;
        page_ = i / per_page_;
      }
    }
  }
  page_ = std::min(page_, page_count() - 1);
}

std::size_t RecoveryBrowser::page_count() const {
  return std::max<std::size_t>(1, (rows_.size() + per_page_ - 1) / per_page_);
}

void RecoveryBrowser::next_page() {
  if (page_ + 1 < page_count()) {
    ++page_;
  }
}

void RecoveryBrowser::previous_page() {
  if (page_ > 0) {
    --page_;
  }
}

const RecoveryRow* RecoveryBrowser::row_on_page(std::size_t index) const {
  const std::size_t at = page_ * per_page_ + index;
  return index < per_page_ && at < rows_.size() ? &rows_[at] : nullptr;
}

void RecoveryBrowser::choose(std::size_t index_on_page) {
  if (row_on_page(index_on_page) != nullptr) {
    chosen_ = page_ * per_page_ + index_on_page;
  }
}

const RecoveryRow* RecoveryBrowser::chosen() const {
  return chosen_ && *chosen_ < rows_.size() ? &rows_[*chosen_] : nullptr;
}

bool RecoveryBrowser::is_chosen(std::size_t index_on_page) const {
  return chosen_ && *chosen_ == page_ * per_page_ + index_on_page;
}

bool RecoveryBrowser::can(RecoveryAction action) const {
  const RecoveryRow* row = chosen();
  if (row == nullptr) {
    return false;
  }
  switch (action) {
  case RecoveryAction::Load:
    return row->can_load;
  case RecoveryAction::Branch:
    return row->can_branch;
  case RecoveryAction::Undo:
    return row->can_undo;
  }
  return false;
}

std::string RecoveryBrowser::confirmation(RecoveryAction action) const {
  const RecoveryRow* row = chosen();
  if (row == nullptr) {
    return {};
  }
  const std::string name = display_name(row->slot);
  switch (action) {
  case RecoveryAction::Load:
    return row->sequence
               ? compat::format("Load version {} of {}? The game in progress "
                                "is kept as a checkpoint first.",
                                *row->sequence, name)
               : compat::format("Load {}? The game in progress is kept as a "
                                "checkpoint first.",
                                name);
  case RecoveryAction::Branch:
    return compat::format(
        "Make a new save, {}, from version {} of {}? {} stays as it is.",
        display_name(branch_name(row->slot, row->sequence.value_or(0), slots_)),
        row->sequence.value_or(0), name, name);
  case RecoveryAction::Undo:
    return compat::format("Undo the last restore of {}? Loading it gives the "
                          "version it gave before. No version is deleted.",
                          name);
  }
  return {};
}

std::string RecoveryBrowser::summary() const {
  const auto versions = static_cast<std::size_t>(std::ranges::count(
      rows_, RecoveryRow::Kind::Version, &RecoveryRow::kind));
  return compat::format("Page {} of {}: {} saves, {} versions", page_ + 1,
                        page_count(), slots_.size(), versions);
}

std::string RecoveryBrowser::path_of(const std::string& slot) const {
  return folder_.empty() ? slot : (folder_ / slot).string();
}

RecoveryBrowser::Outcome RecoveryBrowser::perform(RecoveryAction action,
                                                  GameState& state,
                                                  GameContext& context) {
  Outcome outcome;
  if (!can(action)) {
    return outcome;
  }
  const RecoveryRow row = *chosen();
  const std::string file = path_of(row.slot);
  // Tests point the browser at a folder of their own; the checkpoint goes
  // there too, not to the player's saves.
  const fs::path checkpoint_slot =
      folder_.empty() ? fs::path() : folder_ / checkpoint_filename;
  std::string error;
  std::string notice;
  switch (action) {
  case RecoveryAction::Load:
    outcome.done =
        row.sequence
            ? SaveGameManager::load_version(state, context, file, *row.sequence,
                                            &error, &notice, checkpoint_slot)
            : SaveGameManager::load_over_current(state, context, file, &error,
                                                 &notice, checkpoint_slot);
    break;
  case RecoveryAction::Branch: {
    const std::string name = branch_name(row.slot, *row.sequence, slots_);
    outcome.done = SaveGameManager::restore_as_branch(
        state, context, file, *row.sequence, path_of(name), &error);
    if (outcome.done) {
      notice = "Made " + display_name(name) + ".";
    }
    break;
  }
  case RecoveryAction::Undo:
    outcome.done =
        SaveGameManager::undo_restore(state, context, file, &error);
    if (outcome.done) {
      notice = display_name(row.slot) + " loads the version it did before.";
    }
    break;
  }
  outcome.message = outcome.done ? notice : error;
  if (action != RecoveryAction::Load || !outcome.done) {
    refresh();
  }
  return outcome;
}

} // namespace save
