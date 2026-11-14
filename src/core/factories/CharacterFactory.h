/*
 * SPDX-FileCopyrightText: 2012 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>

#include "core/character/Character.h"

// Each character is made once, the first time any thread asks, and kept.
// Only making one takes a lock, so a game being built on a worker reads them
// alongside the game.
class CharacterFactory {
public:
  // id 0-99: original characters; id 100-199: doubles (reuse portrait from
  // id%100, names from T/DOUBLE.E, stats algorithmically tweaked). The role
  // of the first call for a double decides its job skill.
  static core::character::CharacterData* create_character(
      int id, core::character::JobRole role = core::character::JobRole::CHEF);

private:
  static std::unique_ptr<core::character::CharacterData> make_original(int id);
  static std::unique_ptr<core::character::CharacterData>
  make_double(int id, core::character::JobRole role);
};
