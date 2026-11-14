/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "engine/music/PcmRing.h"

using music::PcmRing;

namespace {

constexpr std::size_t block = PcmRing::BLOCK_FRAMES;

// frames of a counting pattern from first: the frame number in both
// channels (left low 16 bits, right high 16 bits).
std::vector<std::int16_t> counting(std::uint32_t first, std::size_t frames) {
  std::vector<std::int16_t> out(frames * 2);
  for (std::size_t f = 0; f < frames; ++f) {
    const std::uint32_t n = first + static_cast<std::uint32_t>(f);
    out[f * 2] = static_cast<std::int16_t>(n & 0xFFFF);
    out[f * 2 + 1] = static_cast<std::int16_t>(n >> 16);
  }
  return out;
}

std::uint32_t count_at(const std::vector<std::int16_t>& v, std::size_t frame) {
  return static_cast<std::uint16_t>(v[frame * 2]) |
         (static_cast<std::uint32_t>(
              static_cast<std::uint16_t>(v[frame * 2 + 1]))
          << 16);
}

PcmRing::Tag tag(std::uint32_t generation, std::uint32_t track = 1,
                 std::uint64_t frame = 0) {
  return PcmRing::Tag{.generation = generation, .track = track, .frame = frame};
}

} // namespace

TEST_CASE("the ring wraps and keeps the frames in order", "[music][ring]") {
  PcmRing ring(3); // rounded up to 4 blocks
  CHECK(ring.capacity_frames() == 4 * block);
  std::uint32_t written = 0;
  std::uint32_t read = 0;
  // Writes and reads of sizes that do not line up with the blocks, many
  // times round the ring.
  for (int round = 0; round < 50; ++round) {
    const std::size_t w = 100 + static_cast<std::size_t>(round * 37) % 600;
    const std::size_t room = ring.writable_frames();
    const std::size_t n = ring.write(counting(written, w), tag(1));
    CHECK(n == std::min(w, room));
    written += static_cast<std::uint32_t>(n);
    std::vector<std::int16_t> out(
        (50 + static_cast<std::size_t>(round * 53) % 700) * 2);
    const std::size_t got = ring.read(out, 1);
    for (std::size_t f = 0; f < got; ++f) {
      REQUIRE(count_at(out, f) == read + f);
    }
    read += static_cast<std::uint32_t>(got);
  }
  CHECK(read > 4 * block * 5);
  CHECK(ring.frames_read() == read);
}

TEST_CASE("a short read is silence, and an underrun only while producing",
          "[music][ring]") {
  PcmRing ring(4);
  std::vector<std::int16_t> out(512 * 2, std::int16_t{-1});
  CHECK(ring.read(out, 1) == 0);
  CHECK(std::all_of(out.begin(), out.end(),
                    [](std::int16_t s) { return s == 0; }));
  CHECK(ring.underruns() == 0); // nothing was being produced

  ring.producing(1);
  ring.write(counting(7, 100), tag(1));
  std::fill(out.begin(), out.end(), std::int16_t{-1});
  CHECK(ring.read(out, 1) == 100);
  CHECK(count_at(out, 0) == 7);
  CHECK(std::all_of(out.begin() + 200, out.end(),
                    [](std::int16_t s) { return s == 0; }));
  CHECK(ring.underruns() == 1);

  // A pause: generation 2 has no producer, so its silence is expected.
  CHECK(ring.read(out, 2) == 0);
  CHECK(ring.underruns() == 1);
}

TEST_CASE("older generations are dropped unplayed and newer ones wait",
          "[music][ring]") {
  PcmRing ring(8);
  ring.write(counting(0, 3 * block), tag(1));
  ring.write(counting(1000, 2 * block), tag(2));
  std::vector<std::int16_t> out(block * 2);

  // Generation 2 is asked for: generation 1 goes without a sound.
  REQUIRE(ring.read(out, 2) == block);
  CHECK(count_at(out, 0) == 1000);
  CHECK(ring.blocks_dropped() == 3);

  // Generation 3 arrives while 2 still plays: 2 plays to its end, and 3
  // waits until it is asked for.
  ring.write(counting(5000, block), tag(3));
  REQUIRE(ring.read(out, 2) == block);
  CHECK(count_at(out, 0) == 1000 + block);
  CHECK(ring.read(out, 2) == 0);
  CHECK(ring.buffered_frames() == block);
  REQUIRE(ring.read(out, 3) == block);
  CHECK(count_at(out, 0) == 5000);

  // drop_stale() frees the room without playing anything.
  ring.write(counting(0, 4 * block), tag(3));
  ring.drop_stale(4);
  CHECK(ring.buffered_frames() == 0);
  CHECK(ring.writable_frames() == ring.capacity_frames());
}

TEST_CASE("heard() says which track and frame was played last",
          "[music][ring]") {
  PcmRing ring(8);
  ring.write(counting(0, 600), tag(5, 9, 44100));
  std::vector<std::int16_t> out(300 * 2);
  ring.read(out, 5);
  CHECK(ring.heard() == tag(5, 9, 44100 + 300));
  ring.read(out, 5);
  CHECK(ring.heard() == tag(5, 9, 44100 + 600));
}

// Run under ThreadSanitizer (build/tsan). The producer writes chunks of
// changing sizes; the consumer reads blocks of other sizes and checks every
// frame comes in order.
TEST_CASE("producer and consumer threads at different block sizes",
          "[music][ring][threads]") {
  PcmRing ring;
  constexpr std::uint32_t total = 1u << 20;
  std::atomic<bool> done{false};
  std::jthread producer([&] {
    std::mt19937 rng(1);
    std::uint32_t next = 0;
    ring.producing(1);
    while (next < total) {
      const std::size_t want =
          std::min<std::size_t>(1 + rng() % 1500, total - next);
      const std::size_t n = ring.write(counting(next, want), tag(1));
      next += static_cast<std::uint32_t>(n);
      if (n < want) {
        std::this_thread::yield();
      }
    }
    done = true;
  });
  std::mt19937 rng(2);
  std::uint32_t expect = 0;
  bool in_order = true;
  std::vector<std::int16_t> out(1024 * 2);
  while (expect < total) {
    const std::size_t want = 1 + rng() % 1024;
    const std::size_t got = ring.read(std::span(out).first(want * 2), 1);
    for (std::size_t f = 0; f < got; ++f) {
      in_order = in_order && count_at(out, f) == expect + f;
    }
    expect += static_cast<std::uint32_t>(got);
    if (got == 0 && done) {
      break;
    }
  }
  producer.join();
  CHECK(in_order);
  CHECK(expect == total);
}

// The producer lags behind the generation the consumer wants and keeps
// writing for old ones: none of that may ever be read.
TEST_CASE("a late producer's audio is never read", "[music][ring][threads]") {
  PcmRing ring;
  std::atomic<std::uint32_t> wanted{1};
  std::atomic<bool> stop{false};
  std::jthread producer([&] {
    std::mt19937 rng(3);
    std::uint32_t gen = 1;
    while (!stop) {
      // Moves on to the wanted generation only now and then: late.
      if (rng() % 4 == 0) {
        gen = wanted.load();
      }
      // Every sample carries the generation it was made for.
      std::vector<std::int16_t> chunk(512 * 2, static_cast<std::int16_t>(gen));
      ring.write(chunk, tag(gen));
      std::this_thread::yield();
    }
  });
  bool clean = true;
  std::uint64_t frames = 0;
  std::vector<std::int16_t> out(256 * 2);
  for (int i = 0; i < 20000; ++i) {
    if (i % 50 == 0) {
      wanted.fetch_add(1);
    }
    const std::uint32_t gen = wanted.load();
    const std::size_t got = ring.read(out, gen);
    frames += got;
    for (std::size_t s = 0; s < got * 2; ++s) {
      clean = clean && out[s] == static_cast<std::int16_t>(gen);
    }
  }
  stop = true;
  producer.join();
  CHECK(clean);
  CHECK(frames > 0);
  CHECK(ring.blocks_dropped() > 0);
}
