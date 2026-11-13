/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <memory>

#include "engine/gfx/HdImage.h"

namespace gfx {

// Reads a PNG into straight-alpha RGBA texels. Returns null (and logs a
// warning) when the file cannot be read.
[[nodiscard]] std::unique_ptr<HdImage>
load_png(const std::filesystem::path& path);

// Writes straight-alpha RGBA texels as a PNG, creating parent folders.
// Returns false (and logs a warning) on failure.
[[nodiscard]] bool save_png(const HdImage& image,
                            const std::filesystem::path& path);

} // namespace gfx
