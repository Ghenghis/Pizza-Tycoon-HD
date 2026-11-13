/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/gfx/Png.h"

#include <cstring>
#include <system_error>

#include "engine/Logger.h"
#include "engine/SDL_include.h"

namespace gfx {

namespace {

std::string utf8(const std::filesystem::path& p) {
  const auto u8 = p.generic_u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

} // namespace

std::unique_ptr<HdImage> load_png(const std::filesystem::path& path) {
  SDL_Surface* raw = SDL_LoadPNG(utf8(path).c_str());
  if (!raw) {
    Logger::warn("PNG: cannot read {}: {}", utf8(path), SDL_GetError());
    return nullptr;
  }
  SDL_Surface_unique_ptr loaded(raw);
  SDL_Surface_unique_ptr rgba(
      SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA8888));
  if (!rgba) {
    Logger::warn("PNG: cannot convert {}: {}", utf8(path), SDL_GetError());
    return nullptr;
  }
  auto image = std::make_unique<HdImage>(rgba->w, rgba->h);
  if (!SDL_LockSurface(rgba.get())) {
    Logger::warn("PNG: cannot lock {}: {}", utf8(path), SDL_GetError());
    return nullptr;
  }
  for (int y = 0; y < rgba->h; ++y) {
    std::memcpy(image->row(y),
                static_cast<const uint8_t*>(rgba->pixels) +
                    static_cast<std::ptrdiff_t>(y) * rgba->pitch,
                static_cast<std::size_t>(rgba->w) * sizeof(uint32_t));
  }
  SDL_UnlockSurface(rgba.get());
  return image;
}

bool save_png(const HdImage& image, const std::filesystem::path& path) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      Logger::warn("Cannot create {}: {}", utf8(path.parent_path()),
                   ec.message());
      return false;
    }
  }
  SDL_Surface_unique_ptr surface(
      SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA8888,
                            const_cast<uint32_t*>(image.pixels.data()),
                            image.width * static_cast<int>(sizeof(uint32_t))));
  if (!surface) {
    Logger::warn("Cannot wrap image for {}: {}", utf8(path), SDL_GetError());
    return false;
  }
  if (!SDL_SavePNG(surface.get(), utf8(path).c_str())) {
    Logger::warn("Cannot write {}: {}", utf8(path), SDL_GetError());
    return false;
  }
  return true;
}

} // namespace gfx
