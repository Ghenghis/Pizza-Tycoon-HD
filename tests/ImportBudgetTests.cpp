/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/Logger.h"
#include "engine/gfx/Png.h"
#include "resources/HdPack.h"

using namespace gfx;
using resources::HdPack;
using test_support::ScopedHdScale;
using test_support::TempDir;

namespace {

// Leaves the pack switched off for the next test.
struct PackReset {
  ~PackReset() { HdPack::instance().configure({}); }
};

// Collects the log's warnings while it lives. No other test installs a log
// sink, so clearing them all afterwards is safe.
struct WarningLog {
  std::vector<std::string> lines;
  WarningLog() {
    Logger::add_sink([this](LogLevel level, const std::string& msg) {
      if (level == LogLevel::Warning) {
        lines.push_back(msg);
      }
    });
  }
  ~WarningLog() { Logger::clear_sinks(); }
  WarningLog(const WarningLog&) = delete;
  WarningLog& operator=(const WarningLog&) = delete;
  [[nodiscard]] bool mentions(const std::string& text) const {
    for (const std::string& line : lines) {
      if (line.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
};

// The 33 bytes a PNG starts with: the signature and an IHDR chunk. The
// header reader does not check the CRC, so it is left zero.
std::vector<uint8_t> png_header(uint32_t width, uint32_t height,
                                uint8_t depth = 8, uint8_t colour_type = 6) {
  std::vector<uint8_t> b{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
                         0,    0,   0,   13,  'I',  'H',  'D',  'R'};
  for (const uint32_t v : {width, height}) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      b.push_back(static_cast<uint8_t>(v >> shift));
    }
  }
  b.insert(b.end(), {depth, colour_type, 0, 0, 0}); // no interlace
  b.insert(b.end(), {0, 0, 0, 0});                  // CRC
  return b;
}

std::vector<uint8_t> random_bytes(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> out(n);
  for (uint8_t& byte : out) {
    byte = static_cast<uint8_t>(rng());
  }
  return out;
}

void write_file(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A real PNG of noise, so it does not compress to a few bytes and a cut
// lands in the middle of the picture data.
std::vector<uint8_t> noise_png(const std::filesystem::path& scratch, int w,
                               int h) {
  HdImage img(w, h);
  std::mt19937 rng(7);
  for (uint32_t& p : img.pixels) {
    p = static_cast<uint32_t>(rng()) | 0xFFu;
  }
  REQUIRE(save_png(img, scratch));
  return read_file(scratch);
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("PNG headers are read and checked before any decoding",
          "[hd][png][budget]") {
  std::string error;
  const auto info = parse_png_header(png_header(640, 400), &error);
  REQUIRE(info);
  CHECK(info->width == 640);
  CHECK(info->height == 400);

  // The default limits, right at the edge: 16384 per side and 128 Mi texels.
  const PngLimits limits;
  CHECK(check_png_limits({16384, 8192}, 1000).empty());
  CHECK(check_png_limits({8192, 16384}, limits.max_file_bytes).empty());
  CHECK(contains(check_png_limits({16385, 1}, 1000), "per side"));
  CHECK(contains(check_png_limits({1, 16385}, 1000), "per side"));
  CHECK(contains(check_png_limits({16384, 8193}, 1000), "texel limit"));
  CHECK(contains(check_png_limits({4, 4}, limits.max_file_bytes + 1),
                 "byte limit"));
  CHECK(contains(check_png_limits({0, 4}, 1000), "no texels"));

  // Headers that are not a PNG, or not a valid one.
  const auto rejects = [&](const std::vector<uint8_t>& bytes,
                           const std::string& why) {
    error.clear();
    CHECK_FALSE(parse_png_header(bytes, &error));
    CHECK(contains(error, why));
  };
  rejects({}, "too short");
  auto one_short = png_header(4, 4);
  one_short.pop_back();
  rejects(one_short, "too short");
  rejects(random_bytes(64, 1), "no PNG signature");
  auto not_ihdr = png_header(4, 4);
  not_ihdr[12] = 'I';
  not_ihdr[13] = 'D';
  not_ihdr[14] = 'A';
  not_ihdr[15] = 'T';
  rejects(not_ihdr, "not IHDR");
  auto long_ihdr = png_header(4, 4);
  long_ihdr[11] = 14;
  rejects(long_ihdr, "not IHDR");
  rejects(png_header(0, 4), "zero width or height");
  rejects(png_header(4, 0), "zero width or height");
  rejects(png_header(0x80000000u, 4), "out of range");
  rejects(png_header(4, 4, /*depth=*/4, /*colour_type=*/2), "bit depth");
  rejects(png_header(4, 4, 8, /*colour_type=*/5), "bit depth");
  auto interlace = png_header(4, 4);
  interlace[28] = 2;
  rejects(interlace, "interlace");
  // Every legal depth and colour type passes.
  CHECK(parse_png_header(png_header(4, 4, 16, 0)));
  CHECK(parse_png_header(png_header(4, 4, 1, 3)));
  CHECK(parse_png_header(png_header(4, 4, 16, 6)));
}

TEST_CASE("an oversized PNG is turned down from its header alone",
          "[hd][png][budget]") {
  TempDir dir;
  // These claim to be enormous; their "picture data" is a few bytes of
  // noise. Decoding one would ask for gigabytes before finding that out.
  auto wide = png_header(100000, 50000);
  auto many = png_header(16384, 16384); // per side is fine, 1 GiB is not
  const auto tail = random_bytes(256, 2);
  wide.insert(wide.end(), tail.begin(), tail.end());
  many.insert(many.end(), tail.begin(), tail.end());
  write_file(dir.path() / "wide.png", wide);
  write_file(dir.path() / "many.png", many);

  std::string error;
  CHECK_FALSE(read_png_info(dir.path() / "wide.png", {}, &error));
  CHECK(contains(error, "per side"));
  error.clear();
  CHECK_FALSE(load_png(dir.path() / "wide.png", {}, &error));
  CHECK(contains(error, "per side"));
  error.clear();
  CHECK_FALSE(load_png(dir.path() / "many.png", {}, &error));
  CHECK(contains(error, "texel limit"));
  CHECK(contains(error, "many.png"));
}

TEST_CASE("truncated, garbage and missing PNGs fail cleanly",
          "[hd][png][budget]") {
  TempDir dir;
  const auto good = noise_png(dir.path() / "good.png", 64, 32);
  REQUIRE(good.size() > 1000);
  REQUIRE(load_png(dir.path() / "good.png"));

  const auto cut = [&](std::size_t n) {
    return std::vector<uint8_t>(good.begin(),
                                good.begin() + static_cast<std::ptrdiff_t>(n));
  };
  struct Case {
    const char* name;
    std::vector<uint8_t> bytes;
    const char* why;
  };
  const std::vector<Case> cases{
      {"empty.png", {}, "not a PNG"},
      {"stub.png", cut(20), "not a PNG"},
      {"header.png", cut(png_header_bytes), "cannot decode"},
      {"half.png", cut(good.size() / 2), "cannot decode"},
      {"no_end.png", cut(good.size() - 12), "cannot decode"},
      {"garbage.png", random_bytes(4096, 3), "not a PNG"},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    write_file(dir.path() / c.name, c.bytes);
    std::string error;
    CHECK_FALSE(load_png(dir.path() / c.name, {}, &error));
    CHECK(contains(error, c.why));
  }

  std::string error;
  CHECK_FALSE(load_png(dir.path() / "missing.png", {}, &error));
  CHECK(contains(error, "cannot open"));
  CHECK_FALSE(read_png_info(dir.path() / "missing.png", {}, &error));

  // Without an error string the reason goes to the log.
  WarningLog log;
  CHECK_FALSE(load_png(dir.path() / "garbage.png"));
  CHECK(log.mentions("garbage.png"));
}

TEST_CASE("a PNG right at the limits loads and one over does not",
          "[hd][png][budget]") {
  TempDir dir;
  const uint32_t colour = pack_rgba(10, 20, 30, 255);
  REQUIRE(save_png(HdImage(64, 32, colour), dir.path() / "at.png"));
  REQUIRE(save_png(HdImage(65, 32, colour), dir.path() / "wide.png"));
  REQUIRE(save_png(HdImage(64, 33, colour), dir.path() / "tall.png"));
  PngLimits limits;
  limits.max_side = 64;
  limits.max_pixels = 64 * 32;
  limits.max_file_bytes = std::filesystem::file_size(dir.path() / "at.png");

  std::string error;
  const auto at = load_png(dir.path() / "at.png", limits, &error);
  REQUIRE(at);
  CHECK(error.empty());
  CHECK(at->width == 64);
  CHECK(at->height == 32);
  CHECK(at->at(63, 31) == colour);
  const auto info = read_png_info(dir.path() / "at.png", limits);
  REQUIRE(info);
  CHECK(info->width == 64);

  limits.max_file_bytes = 1u << 20;
  CHECK_FALSE(load_png(dir.path() / "wide.png", limits, &error));
  CHECK(contains(error, "per side"));
  CHECK_FALSE(load_png(dir.path() / "tall.png", limits, &error));
  CHECK(contains(error, "texel limit"));

  limits.max_file_bytes = std::filesystem::file_size(dir.path() / "at.png") - 1;
  CHECK_FALSE(load_png(dir.path() / "at.png", limits, &error));
  CHECK(contains(error, "byte limit"));
}

TEST_CASE("pack files that fail to load keep the original art",
          "[hd][pack][budget]") {
  ScopedHdScale scale(2);
  PackReset reset;
  TempDir dir;
  const auto root = dir.path() / "pack";
  // Originals of 4x2 pixels: engine surfaces of 8x4, 16x8 texels at 2x.
  const Size engine{8, 4};
  REQUIRE(save_png(HdImage(16, 8, pack_rgba(1, 2, 3, 255)),
                   root / "gfx" / "good.png"));
  const auto noise = noise_png(dir.path() / "noise.png", 64, 32);
  write_file(root / "gfx" / "cut.png",
             std::vector<uint8_t>(noise.begin(), noise.begin() + 100));
  write_file(root / "gfx" / "garbage.png", random_bytes(2048, 4));
  write_file(root / "gfx" / "huge.png", png_header(40000, 20000));
  // The right shape but cut off, so it fails in the decoder itself.
  write_file(root / "gfx" / "late.png",
             std::vector<uint8_t>(noise.begin(),
                                  noise.begin() + static_cast<std::ptrdiff_t>(
                                                      noise.size() / 2)));

  WarningLog log;
  auto& pack = HdPack::instance();
  pack.configure(root);
  REQUIRE(pack.enabled());
  for (const char* key : {"gfx/cut", "gfx/garbage", "gfx/huge", "gfx/late"}) {
    INFO(key);
    CHECK(pack.vga(key, engine) == nullptr);
  }
  CHECK(pack.vga("gfx/good", engine) != nullptr);
  CHECK(pack.stats().rejected == 4);
  CHECK(pack.stats().loaded == 1);
  CHECK(log.mentions("huge.png"));
  CHECK(log.mentions("late.png"));
  // A failure is remembered, not retried on every use.
  CHECK(pack.vga("gfx/garbage", engine) == nullptr);
  CHECK(pack.stats().rejected == 4);

  // The same files through the background prefetch: the failures are
  // logged on this thread, and loads afterwards come from the cache.
  log.lines.clear();
  pack.configure(root);
  const std::vector<HdPack::WarmEntry> list{{"gfx/cut.png", engine},
                                            {"gfx/garbage.png", engine},
                                            {"gfx/huge.png", engine},
                                            {"gfx/late.png", engine},
                                            {"gfx/good.png", engine}};
  pack.prefetch(list, 1 << 30);
  pack.wait_for_prefetch();
  CHECK(pack.stats().rejected == 4);
  CHECK(pack.stats().loaded == 1);
  CHECK(log.mentions("huge.png"));
  CHECK(log.mentions("garbage.png"));
  CHECK(pack.vga("gfx/late", engine) == nullptr);
  CHECK(pack.vga("gfx/good", engine) != nullptr);
  CHECK(pack.stats().rejected == 4);
  CHECK(pack.take_prefetched().size() == 1);
}

TEST_CASE("a load never waits forever on a prefetch that fails or stops",
          "[hd][pack][budget][threads]") {
  ScopedHdScale scale(2);
  PackReset reset;
  TempDir dir;
  const auto root = dir.path() / "pack";
  const auto noise = noise_png(dir.path() / "noise.png", 1024, 512);
  // Cut late, so the prefetch is busy decoding when the load comes in.
  write_file(root / "gfx" / "big.png",
             std::vector<uint8_t>(
                 noise.begin(),
                 noise.end() - static_cast<std::ptrdiff_t>(noise.size() / 8)));
  REQUIRE(save_png(HdImage(32, 16, pack_rgba(5, 6, 7, 255)),
                   root / "gfx" / "small.png"));
  auto& pack = HdPack::instance();
  pack.configure(root);
  const Size engine{512, 256};

  // Whoever decodes it first, the other gets the same answer: no picture.
  pack.prefetch({{"gfx/big.png", engine}}, 1 << 30);
  CHECK(pack.vga("gfx/big", engine) == nullptr);
  pack.wait_for_prefetch();
  CHECK(pack.stats().rejected == 1); // decoded once

  // Stopping a prefetch part way (configure() does) leaves nothing
  // pending: the pack loads normally afterwards.
  std::vector<HdPack::WarmEntry> many;
  for (int i = 1; i <= 64; ++i) {
    many.push_back({"gfx/small.png", Size{16 * i, 8 * i}});
  }
  pack.configure(root);
  pack.prefetch(many, 1 << 30);
  pack.configure(root);
  CHECK(pack.vga("gfx/small", Size{16, 8}) != nullptr);
  CHECK(pack.vga("gfx/big", engine) == nullptr);
}
