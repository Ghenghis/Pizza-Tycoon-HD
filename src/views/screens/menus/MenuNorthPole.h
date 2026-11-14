/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "views/base/Menu.h"

// The presets of Santa's Pizza Village: Cozy Village, Workshop Rush, Santa's
// Pizza Empire and the sandbox. Each sets up a new game of its scenario and
// goes on to the character screen; the scenario fixes the start.
class MenuNorthPole : public Menu {
public:
  MenuNorthPole(GameContext& context, GameState& state);

private:
  Surface background() const override;
};
