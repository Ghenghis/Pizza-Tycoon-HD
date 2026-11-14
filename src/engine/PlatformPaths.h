/*
 * SPDX-FileCopyrightText: 2026 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>

namespace platform_paths {

// Must be called after SDL_Init(). Resolves the platform-appropriate
// preferences directory via SDL_GetPrefPath and the data directory via
// SDL_GetBasePath.
void init();

const std::string& base_dir();
// The files shipped with the game: next to the executable on Windows and
// Linux, in the bundle's Resources on macOS.
const std::string& data_dir();
// The executable's folder (SDL_GetBasePath), where a portable install may
// keep an Xmas folder of music.
const std::string& game_dir();
std::string config_file_path();
std::string bindings_file_path();
std::string saves_dir();
std::string screenshots_dir();
std::string recipes_dir();
std::string missions_dir();
// The player's music settings, playlists and track cache: never the songs.
std::string music_dir();
// The player's own Music folder as the system names it (Windows' known
// folder, XDG on Linux); empty when there is none.
std::string user_music_dir();

} // namespace platform_paths
