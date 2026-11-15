/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <memory>

#include "core/save/RecoveryBrowser.h"
#include "views/base/Menu.h"
#include "views/ui/BusyCursor.h"

// The Recovery Center: every save and checkpoint with its versions, how far
// each got through its checks, when it was made and its game day, and why
// one can't be used. A chosen version can be loaded, copied into a new save
// or, for a save, its last restore undone; each asks first, and none
// deletes anything. Browsing only reads the save folder; like every menu,
// it stops the game's clock.
class MenuRecovery : public Menu {
public:
  MenuRecovery(GameContext& context, GameState& state);
  void tick() override;
  void render(const RenderContext& ctx) override;

private:
  Surface background() const override;
  void ask(save::RecoveryAction action);
  void run(save::RecoveryAction action);

  int row_height_;
  // Shared with the items that show it.
  std::shared_ptr<save::RecoveryBrowser> browser_;
  views::ui::BusyCursor busy_cursor_;
};
