/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resources/AssetExporter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

#include "engine/Endian.h"
#include "engine/Logger.h"
#include "engine/gfx/HdImage.h"
#include "engine/gfx/Png.h"
#include "engine/gfx/Surface.h"
#include "resources/AssetMetadata.h"
#include "resources/BobLoader.h"
#include "resources/HdPack.h"
#include "resources/PaletteLoader.h"
#include "resources/Upscaling.h"
#include "resources/Vga.h"
#include "resources/VgaLoader.h"
#include "resources/util/FileUtils.h"

namespace resources {

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// The surface is the original doubled with plain pixel doubling, so taking
// the top-left pixel of every 2x2 block gives the original back exactly.
gfx::HdImage to_original_rgba(const gfx::Surface& s, std::size_t& opaque) {
  const int w = s.width() / gfx::asset_pixel_scale;
  const int h = s.height() / gfx::asset_pixel_scale;
  gfx::HdImage out(w, h);
  const auto& pal = s.colors();
  opaque = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t idx = s.get(
          ViewPoint{x * gfx::asset_pixel_scale, y * gfx::asset_pixel_scale});
      if (idx == gfx::TRANSPARENT || idx >= pal.size()) {
        out.at(x, y) = 0;
        continue;
      }
      ++opaque;
      const auto& c = pal[idx];
      out.at(x, y) = gfx::pack_rgba(c.r, c.g, c.b, 255);
    }
  }
  return out;
}

std::optional<std::vector<uint8_t>> read_file(const fs::path& p) {
  std::ifstream is(p, std::ios::binary);
  if (!is) {
    return std::nullopt;
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(is), {});
}

// Checks the header the way VgaLoader will, so a damaged file is skipped
// instead of stopping the export.
bool plausible_vga(const std::vector<uint8_t>& bytes) {
  if (bytes.size() <= VGA_HEADER_SIZE) {
    return false;
  }
  const int w = endian::u16le(bytes[0], bytes[1]);
  const int h = endian::u16le(bytes[2], bytes[3]);
  return w > 0 && h > 0 && w <= 320 && h <= 320;
}

// Checks that every frame's pixels are inside the file.
bool plausible_bob(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 6) {
    return false;
  }
  const std::size_t n = endian::u16le(bytes[0], bytes[1]);
  std::size_t total = 6 + n * 7;
  if (n == 0 || total > bytes.size()) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    total += static_cast<std::size_t>(bytes[6 + i * 7]) * bytes[7 + i * 7];
  }
  return total <= bytes.size();
}

nlohmann::json describe(const std::string& kind, const std::string& key,
                        const std::string& file, const gfx::HdImage& img,
                        std::size_t opaque, std::optional<uint8_t> palette) {
  const auto pixels = static_cast<std::size_t>(img.width) * img.height;
  nlohmann::json j{
      {"kind", kind},
      {"key", key},
      {"file", file},
      {"width", img.width},
      {"height", img.height},
      {"palette", palette.value_or(0)},
      {"palette_known", palette.has_value()},
      {"opaque_pixels", opaque},
      {"transparent_pixels", pixels - opaque},
  };
  return j;
}

} // namespace

ExportReport export_assets(const fs::path& game_root, const fs::path& out_dir) {
  ExportReport report;
  fileutils::game_data_path(game_root.string());
  load_palette_bank();

  // Exact doubling, and no HD pack, so the PNGs are the original pixels.
  const Upscaling saved_upscaling = upscaling();
  upscaling({.pictures = gfx::Upscaler::Double, .text = gfx::Upscaler::Double});
  const fs::path saved_pack = HdPack::instance().root();
  HdPack::instance().configure({});

  std::vector<fs::path> files;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(game_root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::string ext = lower(it->path().extension().string());
    if (ext == ".vga" || ext == ".bob") {
      files.push_back(fs::relative(it->path(), game_root, ec));
    }
  }
  std::sort(files.begin(), files.end());

  nlohmann::json entries = nlohmann::json::array();
  VgaLoader vga_loader;
  BobLoader bob_loader;

  for (const fs::path& rel : files) {
    const std::string rel_text = rel.generic_string();
    const std::string ext = lower(rel.extension().string());
    const auto bytes = read_file(game_root / rel);
    if (!bytes) {
      report.warnings.push_back("cannot read " + rel_text);
      ++report.failed;
      continue;
    }

    // The engine loads these with lower-case keys: "gfx/back1" for VGA
    // (extension added by the loader), "bob/moebel.bob" for BOB.
    std::string stem = lower(rel_text.substr(0, rel_text.size() - ext.size()));

    if (ext == ".vga") {
      if (!plausible_vga(*bytes)) {
        report.warnings.push_back("not a VGA picture, skipped: " + rel_text);
        ++report.skipped;
        continue;
      }
      const gfx::Surface surface = vga_loader.load(stem);
      std::size_t opaque = 0;
      const gfx::HdImage img = to_original_rgba(surface, opaque);
      const std::string file = HdPack::vga_relative_path(stem);
      if (!gfx::save_png(img, out_dir / file)) {
        ++report.failed;
        continue;
      }
      entries.push_back(describe("vga", HdPack::normalize_key(stem), file, img,
                                 opaque,
                                 AssetMetadataRegistry::get_palette(stem)));
      ++report.vga_files;
    }
    else {
      if (!plausible_bob(*bytes)) {
        report.warnings.push_back("not a BOB sprite file, skipped: " +
                                  rel_text);
        ++report.skipped;
        continue;
      }
      const std::string key = stem + ".bob";
      const auto bob = bob_loader.load(key);
      const auto palette = AssetMetadataRegistry::get_palette(key);
      for (std::size_t f = 0; f < bob->frame_count(); ++f) {
        if (bob->width(f) == 0 || bob->height(f) == 0) {
          // Empty frames exist in the originals; nothing to replace.
          continue;
        }
        const gfx::Surface surface = bob->frame(f);
        std::size_t opaque = 0;
        const gfx::HdImage img = to_original_rgba(surface, opaque);
        const std::string file = HdPack::bob_frame_relative_path(key, f);
        if (!gfx::save_png(img, out_dir / file)) {
          ++report.failed;
          continue;
        }
        auto entry = describe("bob", HdPack::normalize_key(key), file, img,
                              opaque, palette);
        entry["frame"] = f;
        entries.push_back(std::move(entry));
        ++report.bob_frames;
      }
      ++report.bob_files;
    }
  }

  nlohmann::json manifest{
      {"format", 1},
      {"generator", "Pizza Legacy HD asset export"},
      {"engine_pixel_scale", gfx::asset_pixel_scale},
      {"note",
       "Each file is the original picture at its original size. To make an "
       "HD pack, copy this layout and replace each PNG with a larger one "
       "of the same aspect ratio (12x the original matches hd_scale 6, "
       "3840x2400). Alpha 0 marks the transparent colour."},
      {"images", entries},
  };
  fs::create_directories(out_dir, ec);
  std::ofstream out(out_dir / "manifest.json");
  out << manifest.dump(1) << "\n";
  if (!out) {
    report.warnings.push_back("could not write manifest.json");
    ++report.failed;
  }

  upscaling(saved_upscaling);
  HdPack::instance().configure(saved_pack);
  return report;
}

} // namespace resources
