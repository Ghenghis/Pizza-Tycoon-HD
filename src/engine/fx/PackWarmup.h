/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <vector>
#include "resources/HdPack.h"

// What the HD pack decodes in the background at start-up, so a screen's art
// is ready when the screen first opens instead of being decoded on its first
// frame (a 4K background is 100-150 ms of PNG decoding).
//
// The warm list (hd_warm.txt next to the settings) only exists from the
// second session, so the first session used to decode every picture on the
// frame that first showed it. Now the list for a session is, in order:
//   1. the player's warm list: what the last session used, in its order;
//   2. the pack's own list, <pack>/hd_warm.txt, if the pack ships one (a
//      reference session's warm list, same format);
//   3. the VGA pictures the pack replaces (<pack>/gfx/*.png), with their
//      engine sizes read from the originals' headers, biggest first (the
//      pictures that stall the longest), up to `scan_budget` bytes of
//      decoded pictures at hd scale k. Prefetched pictures are also
//      streamed to the GPU and enter the next warm list, so the scan is
//      kept to the pictures most worth it.
// Each picture once, at its first place. HdPack::prefetch() skips what is
// already decoded or being decoded, so the list can be handed over again
// once the game's folder is known.
namespace fx {

using WarmEntry = resources::HdPack::WarmEntry;

// (3) alone: the pack's VGA replacements whose originals are in game_root
// (case-insensitive), biggest first, then by name. Pictures without an
// original, or whose shape differs from it (the pack would reject them), are
// left out.
[[nodiscard]] std::vector<WarmEntry>
scan_pack_pictures(const std::filesystem::path& pack_root,
                   const std::filesystem::path& game_root);

inline constexpr std::size_t default_scan_budget = std::size_t{512} << 20;

// The whole list, (1) to (3). An empty game_root leaves (3) out.
[[nodiscard]] std::vector<WarmEntry>
session_warm_list(const std::filesystem::path& user_list,
                  const std::filesystem::path& pack_root,
                  const std::filesystem::path& game_root, int k,
                  std::size_t scan_budget = default_scan_budget);

} // namespace fx
