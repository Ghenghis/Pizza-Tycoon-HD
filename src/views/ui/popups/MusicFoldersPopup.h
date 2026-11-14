/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "engine/music/MusicSystem.h"
#include "views/ui/popups/BasePopup.h"
#include "views/ui/popups/PopupButton.h"

class PopupManager;

// The Music folders screen, on MusicSystem's folder draft: turn folders on
// or off, with or without their subfolders, rename, locate a moved one,
// remove, add, rescan, reset to the suggested ones. Nothing changes for the
// game until Save; Cancel (Esc) drops the changes. The game runs on while it
// is open.
class MusicFoldersPopup : public BasePopup {
public:
  // on_close runs once when the screen closes, from Save or Cancel (not
  // when it closes for a rename, which opens it again).
  explicit MusicFoldersPopup(const GameContext& ctx,
                             std::function<void()> on_close = {});

  void tick() override;
  void render(Surface& surface, ViewPoint origin) override;
  void add_interactions(views::ui::InteractionContext ictx,
                        ViewPoint origin) override;
  Size popup_size() const override { return {width, height}; }
  bool keeps_game_running() const override { return true; }

  static constexpr int width = 560;
  static constexpr int height = 254;
  static constexpr int rows_per_page = 6;

private:
  struct RowButtons {
    PopupButton enabled;
    PopupButton subfolders;
    PopupButton rename;
    PopupButton locate;
    PopupButton remove;
  };

  void refresh();
  void close(bool save);
  void rename(std::size_t slot);
  [[nodiscard]] const music::MusicSystem::FolderRow*
  row(std::size_t slot) const;
  [[nodiscard]] std::size_t pages() const;
  [[nodiscard]] static RowButtons make_row(MusicFoldersPopup& self, int slot);

  music::MusicSystem* music_;
  PopupManager* popups_;
  std::function<void()> on_close_;

  std::vector<music::MusicSystem::FolderRow> rows_;
  std::size_t page_ = 0;
  std::string message_;
  std::string seen_folder_message_;
  std::chrono::steady_clock::time_point next_refresh_{};

  std::array<RowButtons, rows_per_page> row_buttons_;
  PopupButton add_;
  PopupButton rescan_;
  PopupButton reset_;
  PopupButton page_prev_;
  PopupButton page_next_;
  PopupButton cancel_;
  PopupButton save_;
};
