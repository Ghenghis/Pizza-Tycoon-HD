/*
 * SPDX-FileCopyrightText: 2010 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "views/base/Menu.h"

class MenuMain : public Menu {
public:
  MenuMain(GameContext& context, GameState& state);

protected:
  Surface background() const override;

private:
  // After a run that did not end cleanly: offers, once, to resume its last
  // verified timed checkpoint. The player chooses; nothing loads by itself.
  void offer_resume();
};
