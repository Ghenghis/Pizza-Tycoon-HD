/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <map>
#include <string>
#include "core/state/GameOptions.h"

class ConfigManager {
public:
  explicit ConfigManager(std::string filename = "./settings.conf");

  bool load_config();

  // Rewrites the settings file. Returns false, logging why and leaving the
  // old file in place, if it could not be written.
  bool save_config();
  const std::string value(const std::string& key) const;
  void value(const std::string& key, const std::string& value);
  // Stores the options that differ from the defaults, then save_config().
  bool save_options(const GameOptions& opts);
  GameOptions load_options() const;

  std::string active_game_path() const;
  void active_game_path(const std::string& path);

private:
  std::string filename_;
  std::map<std::string, std::string> config_;
};
