/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace resources {

struct ExportReport {
  std::size_t vga_files = 0;
  std::size_t bob_files = 0;
  std::size_t bob_frames = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::vector<std::string> warnings;
};

// Writes every VGA picture and every BOB frame of the original game as a PNG
// at its original size, decoded with the engine's own loaders and with the
// palette the engine uses for it (index 0, the transparent colour, becomes
// alpha 0). The folder layout is the HD pack layout (see HdPack.h), so a pack
// is made by replacing each PNG with a bigger one.
//
// Also writes manifest.json describing every image: kind, key, file, size,
// palette, and how much of it is transparent.
//
// Sets the game data path to game_root. Call after SDL is available; it
// does not need a window.
[[nodiscard]] ExportReport export_assets(const std::filesystem::path& game_root,
                                         const std::filesystem::path& out_dir);

} // namespace resources
