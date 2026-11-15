/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the Recovery Center shows and does, apart from drawing it: the saves
// and checkpoints as rows, paged, one row chosen, and the three actions on
// it. Browsing only reads the save folder; nothing here touches the game
// until an action the player confirmed runs.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/save/SaveGameManager.h"

class GameState;
class GameContext;

namespace save {

enum class RecoveryAction {
  Load,   // load this version (or this save, if it keeps none)
  Branch, // copy this version into a new save
  Undo,   // undo the last restore of this save
};

// One line of the list.
struct RecoveryRow {
  enum class Kind {
    Slot,    // a save, with what it keeps
    Version, // one of its versions
  };
  Kind kind = Kind::Slot;
  std::string slot; // the save's file name
  std::optional<std::uint64_t> sequence;
  std::string text;   // the line itself
  std::string detail; // what the detail box says about it
  bool checkpoint = false; // a save the game makes on its own
  bool active = false;     // the version loading the save gives
  bool damaged = false;    // not usable, and detail says why
  bool can_load = false;
  bool can_branch = false;
  bool can_undo = false;
};

// The slots as rows: checkpoints first, then the other saves by name; each
// save's own row, then its versions, newest first.
[[nodiscard]] std::vector<RecoveryRow>
recovery_rows(const std::vector<RecoverySlot>& slots);

// A name for a branch of a version that no save has yet: "MYGAME-12.plsave",
// then "MYGAME-12b.plsave" and on.
[[nodiscard]] std::string branch_name(std::string_view slot,
                                      std::uint64_t sequence,
                                      const std::vector<RecoverySlot>& slots);

class RecoveryBrowser {
public:
  // The saves folder if folder is empty.
  explicit RecoveryBrowser(std::size_t rows_per_page,
                           std::filesystem::path folder = {});

  // Reads the folder again. The chosen row stays chosen if it is still
  // there.
  void refresh();

  [[nodiscard]] const std::vector<RecoveryRow>& rows() const { return rows_; }
  [[nodiscard]] std::size_t rows_per_page() const { return per_page_; }
  [[nodiscard]] std::size_t page() const { return page_; }
  [[nodiscard]] std::size_t page_count() const;
  void next_page();
  void previous_page();

  // A row of the current page, or nothing past its end.
  [[nodiscard]] const RecoveryRow* row_on_page(std::size_t index) const;
  void choose(std::size_t index_on_page);
  [[nodiscard]] const RecoveryRow* chosen() const;
  [[nodiscard]] bool is_chosen(std::size_t index_on_page) const;

  [[nodiscard]] bool can(RecoveryAction action) const;
  // The question the player answers before the action runs.
  [[nodiscard]] std::string confirmation(RecoveryAction action) const;
  // "Page 2 of 3: 6 saves, 14 versions".
  [[nodiscard]] std::string summary() const;

  struct Outcome {
    bool done = false;
    std::string message; // for the player; empty if there is nothing to say
  };
  // Runs a confirmed action on the chosen row. A load replaces the game in
  // progress, keeping it as a checkpoint first; the others change only save
  // files, and none deletes any.
  Outcome perform(RecoveryAction action, GameState& state,
                  GameContext& context);

private:
  [[nodiscard]] std::string path_of(const std::string& slot) const;

  std::size_t per_page_;
  std::filesystem::path folder_;
  std::vector<RecoverySlot> slots_;
  std::vector<RecoveryRow> rows_;
  std::size_t page_ = 0;
  std::optional<std::size_t> chosen_; // index into rows_
};

} // namespace save
