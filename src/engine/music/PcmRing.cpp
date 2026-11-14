/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/PcmRing.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace music {

// The audio callback relies on these never taking a lock.
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

PcmRing::PcmRing(std::size_t blocks)
    : blocks_(std::bit_ceil(std::max<std::size_t>(blocks, 2))),
      mask_(blocks_ - 1), ring_(new Block[blocks_]()) {}

std::size_t PcmRing::writable_frames() const noexcept {
  const std::size_t w = write_.load(std::memory_order_relaxed);
  const std::size_t r = read_.load(std::memory_order_acquire);
  return (blocks_ - (w - r)) * BLOCK_FRAMES;
}

std::size_t PcmRing::write(std::span<const std::int16_t> samples,
                           Tag tag) noexcept {
  const std::size_t total = samples.size() / CHANNELS;
  std::size_t w = write_.load(std::memory_order_relaxed);
  const std::size_t r = read_.load(std::memory_order_acquire);
  std::size_t done = 0;
  while (done < total && w - r < blocks_) {
    Block& b = ring_[w & mask_];
    const std::size_t n = std::min(total - done, BLOCK_FRAMES);
    b.tag = tag;
    b.tag.frame += done;
    b.frames = static_cast<std::uint32_t>(n);
    std::memcpy(b.samples, samples.data() + done * CHANNELS,
                n * CHANNELS * sizeof(std::int16_t));
    done += n;
    ++w;
  }
  // Publishes the blocks: the consumer reads them only after this store.
  write_.store(w, std::memory_order_release);
  return done;
}

void PcmRing::producing(std::uint32_t generation) noexcept {
  producing_.store(generation, std::memory_order_release);
}

std::size_t PcmRing::skip_stale(std::size_t r, std::size_t w,
                                std::uint32_t generation) noexcept {
  std::uint64_t dropped = 0;
  while (r != w && ring_[r & mask_].tag.generation < generation) {
    ++r;
    ++dropped;
    offset_ = 0;
  }
  if (dropped > 0) {
    blocks_dropped_.fetch_add(dropped, std::memory_order_relaxed);
  }
  return r;
}

void PcmRing::drop_stale(std::uint32_t generation) noexcept {
  const std::size_t w = write_.load(std::memory_order_acquire);
  const std::size_t r = read_.load(std::memory_order_relaxed);
  const std::size_t next = skip_stale(r, w, generation);
  if (next != r) {
    read_.store(next, std::memory_order_release);
  }
}

std::size_t PcmRing::read(std::span<std::int16_t> out,
                          std::uint32_t generation) noexcept {
  const std::size_t want = out.size() / CHANNELS;
  const std::size_t w = write_.load(std::memory_order_acquire);
  std::size_t r = read_.load(std::memory_order_relaxed);
  std::size_t done = 0;
  Tag last{};
  bool any = false;
  while (done < want) {
    r = skip_stale(r, w, generation);
    // Empty, or the next block is for a generation not asked for yet.
    if (r == w || ring_[r & mask_].tag.generation != generation) {
      break;
    }
    const Block& b = ring_[r & mask_];
    const std::size_t n =
        std::min<std::size_t>(b.frames - offset_, want - done);
    std::memcpy(out.data() + done * CHANNELS, b.samples + offset_ * CHANNELS,
                n * CHANNELS * sizeof(std::int16_t));
    done += n;
    offset_ += static_cast<std::uint32_t>(n);
    last = b.tag;
    last.frame += offset_;
    any = true;
    if (offset_ == b.frames) {
      ++r;
      offset_ = 0;
    }
  }
  // Hands the finished blocks back to the producer.
  read_.store(r, std::memory_order_release);

  std::fill(out.begin() + static_cast<std::ptrdiff_t>(done * CHANNELS),
            out.end(), std::int16_t{0});
  if (done < want && producing_.load(std::memory_order_acquire) == generation &&
      generation != 0) {
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }
  if (any) {
    frames_read_.fetch_add(done, std::memory_order_relaxed);
    heard_id_.store((std::uint64_t{last.generation} << 32) | last.track,
                    std::memory_order_relaxed);
    heard_frame_.store(last.frame, std::memory_order_relaxed);
  }
  return done;
}

std::size_t PcmRing::buffered_frames() const noexcept {
  const std::size_t r = read_.load(std::memory_order_acquire);
  const std::size_t w = write_.load(std::memory_order_acquire);
  // Whole blocks: close enough for a fill level.
  return (w - r) * BLOCK_FRAMES;
}

PcmRing::Tag PcmRing::heard() const noexcept {
  const std::uint64_t id = heard_id_.load(std::memory_order_relaxed);
  return Tag{
      .generation = static_cast<std::uint32_t>(id >> 32),
      .track = static_cast<std::uint32_t>(id),
      .frame = heard_frame_.load(std::memory_order_relaxed),
  };
}

std::uint32_t PcmRing::underruns() const noexcept {
  return underruns_.load(std::memory_order_relaxed);
}

std::uint64_t PcmRing::frames_read() const noexcept {
  return frames_read_.load(std::memory_order_relaxed);
}

std::uint64_t PcmRing::blocks_dropped() const noexcept {
  return blocks_dropped_.load(std::memory_order_relaxed);
}

} // namespace music
