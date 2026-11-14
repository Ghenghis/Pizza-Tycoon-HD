/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/gfx/Png.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/FormatCompat.h"
#include "engine/Logger.h"
#include "engine/SDL_include.h"

namespace gfx {

namespace {

std::string utf8(const std::filesystem::path& p) {
  const auto u8 = p.generic_u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

struct IoCloser {
  void operator()(SDL_IOStream* io) const noexcept { SDL_CloseIO(io); }
};
using IoPtr = std::unique_ptr<SDL_IOStream, IoCloser>;

constexpr std::array<std::uint8_t, 8> png_signature{0x89, 'P',  'N',  'G',
                                                    '\r', '\n', 0x1A, '\n'};

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
  return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
         (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

// The bit depths the PNG specification allows for each colour type.
bool valid_depth(std::uint8_t colour_type, std::uint8_t depth) {
  switch (colour_type) {
  case 0: // grey
    return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  case 3: // palette
    return depth == 1 || depth == 2 || depth == 4 || depth == 8;
  case 2: // RGB
  case 4: // grey and alpha
  case 6: // RGBA
    return depth == 8 || depth == 16;
  default:
    return false;
  }
}

// Hands a failure to *error, or to the log when there is none.
void fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  else {
    Logger::warn("PNG: {}", message);
  }
}

// An open PNG file whose header has passed the limits.
struct OpenPng {
  IoPtr io;
  std::uintmax_t file_bytes = 0;
  PngInfo info;
  std::array<std::uint8_t, png_header_bytes> header{};
};

// Opens a PNG and checks its length and header against the limits, reading
// nothing past the header: the first step of read_png_info and load_png.
std::optional<OpenPng> open_png(const std::filesystem::path& path,
                                const PngLimits& limits, std::string* error) {
  const std::string name = utf8(path);
  OpenPng png;
  png.io.reset(SDL_IOFromFile(name.c_str(), "rb"));
  if (!png.io) {
    fail(error, compat::format("{}: cannot open ({})", name, SDL_GetError()));
    return std::nullopt;
  }
  const Sint64 size = SDL_GetIOSize(png.io.get());
  if (size < 0) {
    fail(error,
         compat::format("{}: cannot tell its size ({})", name, SDL_GetError()));
    return std::nullopt;
  }
  png.file_bytes = static_cast<std::uintmax_t>(size);
  const std::size_t got =
      SDL_ReadIO(png.io.get(), png.header.data(), png.header.size());
  std::string reason;
  const auto info = parse_png_header(
      std::span<const std::uint8_t>(png.header.data(), got), &reason);
  if (!info) {
    fail(error, compat::format("{}: not a PNG ({})", name, reason));
    return std::nullopt;
  }
  if (std::string over = check_png_limits(*info, png.file_bytes, limits);
      !over.empty()) {
    fail(error, compat::format("{}: {}", name, over));
    return std::nullopt;
  }
  png.info = *info;
  return png;
}

} // namespace

std::optional<PngInfo> parse_png_header(std::span<const std::uint8_t> bytes,
                                        std::string* error) {
  const auto bad = [error](const char* why) -> std::optional<PngInfo> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };
  if (bytes.size() < png_header_bytes) {
    return bad("too short for a PNG header");
  }
  if (!std::equal(png_signature.begin(), png_signature.end(), bytes.begin())) {
    return bad("no PNG signature");
  }
  // The first chunk must be IHDR, which is 13 bytes long.
  if (be32(bytes, 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
    return bad("the first chunk is not IHDR");
  }
  const std::uint32_t width = be32(bytes, 16);
  const std::uint32_t height = be32(bytes, 20);
  if (width == 0 || height == 0) {
    return bad("zero width or height");
  }
  constexpr auto max_dimension =
      static_cast<std::uint32_t>(std::numeric_limits<int>::max());
  if (width > max_dimension || height > max_dimension) {
    return bad("width or height out of range");
  }
  if (!valid_depth(bytes[25], bytes[24])) {
    return bad("invalid bit depth or colour type");
  }
  if (bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1) {
    return bad("unknown compression, filter or interlace method");
  }
  return PngInfo{static_cast<int>(width), static_cast<int>(height)};
}

std::string check_png_limits(const PngInfo& info, std::uintmax_t file_bytes,
                             const PngLimits& limits) {
  if (file_bytes > limits.max_file_bytes) {
    return compat::format("{} bytes is over the {} byte limit", file_bytes,
                          limits.max_file_bytes);
  }
  if (info.width <= 0 || info.height <= 0) {
    return compat::format("{}x{} has no texels", info.width, info.height);
  }
  if (info.width > limits.max_side || info.height > limits.max_side) {
    return compat::format("{}x{} is over the {} texels per side limit",
                          info.width, info.height, limits.max_side);
  }
  const std::uint64_t pixels = static_cast<std::uint64_t>(info.width) *
                               static_cast<std::uint64_t>(info.height);
  if (pixels > limits.max_pixels) {
    return compat::format("{}x{} is {} texels, over the {} texel limit",
                          info.width, info.height, pixels, limits.max_pixels);
  }
  return {};
}

std::optional<PngInfo> read_png_info(const std::filesystem::path& path,
                                     const PngLimits& limits,
                                     std::string* error) {
  auto png = open_png(path, limits, error);
  if (!png) {
    return std::nullopt;
  }
  return png->info;
}

std::unique_ptr<HdImage> load_png(const std::filesystem::path& path,
                                  const PngLimits& limits, std::string* error) {
  auto png = open_png(path, limits, error);
  if (!png) {
    return nullptr;
  }
  const std::string name = utf8(path);
  try {
    // The header is within the limits: read the rest and decode it.
    if (png->file_bytes < png_header_bytes ||
        png->file_bytes > std::numeric_limits<std::size_t>::max()) {
      fail(error, compat::format("{}: changed while it was read", name));
      return nullptr;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(png->file_bytes));
    std::copy(png->header.begin(), png->header.end(), bytes.begin());
    const std::size_t rest = bytes.size() - png_header_bytes;
    if (SDL_ReadIO(png->io.get(), bytes.data() + png_header_bytes, rest) !=
        rest) {
      fail(error, compat::format("{}: cannot read all of it ({})", name,
                                 SDL_GetError()));
      return nullptr;
    }
    png->io.reset();

    SDL_Surface_unique_ptr loaded(SDL_LoadPNG_IO(
        SDL_IOFromConstMem(bytes.data(), bytes.size()), /*closeio=*/true));
    if (!loaded) {
      fail(error,
           compat::format("{}: cannot decode ({})", name, SDL_GetError()));
      return nullptr;
    }
    std::vector<std::uint8_t>().swap(bytes); // free it before converting
    if (loaded->w != png->info.width || loaded->h != png->info.height) {
      fail(error, compat::format("{}: decoded to {}x{}, not the {}x{} its "
                                 "header says",
                                 name, loaded->w, loaded->h, png->info.width,
                                 png->info.height));
      return nullptr;
    }
    SDL_Surface_unique_ptr rgba(
        SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA8888));
    loaded.reset();
    if (!rgba) {
      fail(error,
           compat::format("{}: cannot convert ({})", name, SDL_GetError()));
      return nullptr;
    }
    auto image = std::make_unique<HdImage>(rgba->w, rgba->h);
    if (!SDL_LockSurface(rgba.get())) {
      fail(error, compat::format("{}: cannot lock ({})", name, SDL_GetError()));
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
  catch (const std::bad_alloc&) {
    fail(error, compat::format("{}: not enough memory to load it", name));
    return nullptr;
  }
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
