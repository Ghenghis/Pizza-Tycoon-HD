/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include <nlohmann/json.hpp>

#include "TestSupport.h"
#include "engine/gfx/Png.h"
#include "resources/AssetExporter.h"

using test_support::TempDir;

TEST_CASE("export writes every original picture and frame", "[export][game]") {
  const auto game = test_support::game_path();
  if (!game) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  TempDir out;
  const auto report = resources::export_assets(*game, out.path());
  CHECK(report.failed == 0);
  // The English DOS release: 145 VGA pictures, 51 BOB files.
  CHECK(report.vga_files == 145);
  CHECK(report.bob_files == 51);
  CHECK(report.bob_frames > 1400);

  std::ifstream in(out.path() / "manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  CHECK(manifest["format"] == 1);
  const auto& images = manifest["images"];
  CHECK(images.size() == report.vga_files + report.bob_frames);

  bool saw_back1 = false;
  for (const auto& img : images) {
    const auto file = out.path() / img["file"].get<std::string>();
    REQUIRE(std::filesystem::exists(file));
    if (img["key"] == "gfx/back1") {
      saw_back1 = true;
      CHECK(img["width"] == 192);
      CHECK(img["height"] == 176);
      CHECK(img["palette"] == 6);
      auto png = gfx::load_png(file);
      REQUIRE(png);
      CHECK(png->width == 192);
      CHECK(png->height == 176);
    }
  }
  CHECK(saw_back1);
}
