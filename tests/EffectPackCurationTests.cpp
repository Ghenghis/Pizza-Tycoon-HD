/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A curated effects pack, checked by the real loader (audio::load_effect_pack)
// against the real originals. Not run by default: it needs the original game
// and a pack folder, neither of which is in the repo.
//
//   PLEGACY_TEST_GAME_PATH=<folder with PT.EXE> PLEGACY_SFX_PACK=<pack folder>
//     test_plegacy "[sfx-pack]"
//
// Prints one line per manifest entry: replaced, or the loader's reason.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "core/content/Sha256.h"
#include "engine/audio/AudioManager.h"
#include "engine/audio/EffectPack.h"
#include "resources/MFileLoader.h"

namespace fs = std::filesystem;

namespace {

// The originals' source hashes by effect slot, as AudioManager makes them:
// every VOC entry of sound/m0 .. m11, in load order, hashed as stored.
std::vector<std::string> original_sources(const fs::path& game) {
  std::vector<std::string> sources;
  for (int m = 0; m <= 11; ++m) {
    const fs::path file = game / "SOUND" / ("M" + std::to_string(m));
    for (const AudioAsset& asset : MFileLoader::load(file.string())) {
      if (asset.type == AudioAssetType::VOC) {
        sources.push_back(core::content::Sha256::hex(
            std::string_view(reinterpret_cast<const char*>(asset.data.data()),
                             asset.data.size())));
      }
    }
  }
  return sources;
}

} // namespace

TEST_CASE("a curated effects pack through the real loader",
          "[.][sfx-pack][audio]") {
  const auto game = test_support::game_path();
  const char* pack_env = std::getenv("PLEGACY_SFX_PACK");
  if (!game || !pack_env || !*pack_env) {
    SKIP("needs PLEGACY_TEST_GAME_PATH and PLEGACY_SFX_PACK");
  }
  const auto sources = original_sources(*game);
  REQUIRE(sources.size() == static_cast<std::size_t>(audio::SoundId::COUNT));

  std::string error;
  const auto pack = audio::load_effect_pack(fs::path(pack_env), sources,
                                            audio::EffectLimits{}, &error);
  INFO(error);
  REQUIRE(pack.has_value());
  std::printf("[sfx-pack] %s revision %u: %zu entries\n", pack->id.c_str(),
              pack->revision, pack->outcomes.size());
  int refused = 0;
  for (const audio::EffectOutcome& o : pack->outcomes) {
    std::printf("[sfx-pack] %-20s slot %2d  %s\n", o.event.c_str(), o.slot,
                o.replaced ? "replaced" : ("REFUSED: " + o.reason).c_str());
    refused += o.replaced ? 0 : 1;
  }
  CHECK(refused == 0);
}
