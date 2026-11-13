/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>

#include "engine/gfx/HdImage.h"

namespace test_support {

// Sets gfx::hd_scale for one test and puts it back afterwards.
class ScopedHdScale {
public:
  explicit ScopedHdScale(int scale) : saved_(gfx::hd_scale()) {
    gfx::hd_scale(scale);
  }
  ~ScopedHdScale() { gfx::hd_scale(saved_); }
  ScopedHdScale(const ScopedHdScale&) = delete;
  ScopedHdScale& operator=(const ScopedHdScale&) = delete;

private:
  int saved_;
};

// A fresh, empty folder under the system temp dir, removed afterwards.
class TempDir {
public:
  TempDir() {
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() /
            ("plegacy-test-" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

// Folder with the original game (the one holding PT.EXE), from the
// PLEGACY_TEST_GAME_PATH environment variable. The original assets are not
// redistributable, so tests that need them SKIP without it.
inline std::optional<std::filesystem::path> game_path() {
  const char* env = std::getenv("PLEGACY_TEST_GAME_PATH");
  if (!env || !*env) {
    return std::nullopt;
  }
  std::filesystem::path p(env);
  if (!std::filesystem::exists(p / "PT.EXE") &&
      !std::filesystem::exists(p / "pt.exe")) {
    return std::nullopt;
  }
  return p;
}

} // namespace test_support
