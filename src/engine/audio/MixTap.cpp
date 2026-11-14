/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/audio/MixTap.h"

#include <algorithm>
#include <bit>

namespace audio {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

MixTap::MixTap(std::size_t blocks)
    : blocks_(std::bit_ceil(std::max<std::size_t>(blocks, 2))),
      ring_(std::make_unique<Block[]>(blocks_)) {}

void MixTap::enabled(bool on) noexcept {
  enabled_.store(on, std::memory_order_relaxed);
}

bool MixTap::enabled() const noexcept {
  return enabled_.load(std::memory_order_relaxed);
}

void MixTap::write(std::span<const std::int16_t> samples) noexcept {
  const std::size_t frames = samples.size() / CHANNELS;
  const std::uint64_t at = position_.load(std::memory_order_relaxed);
  position_.store(at + frames, std::memory_order_release);
  if (!enabled()) {
    return;
  }
  std::uint64_t head = head_.load(std::memory_order_relaxed);
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t n = std::min(BLOCK_FRAMES, frames - done);
    if (head - tail_.load(std::memory_order_acquire) >= blocks_) {
      // Full: the rest of this write is lost, and counted.
      dropped_.fetch_add(frames - done, std::memory_order_relaxed);
      break;
    }
    Block& b = ring_[head & (blocks_ - 1)];
    b.first = at + done;
    b.frames = n;
    std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(done * CHANNELS),
                n * CHANNELS, b.samples);
    ++head;
    head_.store(head, std::memory_order_release);
    kept_.fetch_add(n, std::memory_order_relaxed);
    done += n;
  }
}

std::size_t MixTap::read(std::span<std::int16_t> out,
                         std::uint64_t* first) noexcept {
  const std::size_t room = out.size() / CHANNELS;
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  std::uint64_t tail = tail_.load(std::memory_order_relaxed);
  std::size_t got = 0;
  std::uint64_t next = 0;
  while (got < room && tail != head) {
    const Block& b = ring_[tail & (blocks_ - 1)];
    const std::uint64_t at = b.first + offset_;
    if (got > 0 && at != next) {
      break; // a gap: the next run starts there
    }
    if (got == 0 && first) {
      *first = at;
    }
    const std::size_t n = std::min(room - got, b.frames - offset_);
    std::copy_n(b.samples + offset_ * CHANNELS, n * CHANNELS,
                out.begin() + static_cast<std::ptrdiff_t>(got * CHANNELS));
    got += n;
    next = at + n;
    offset_ += n;
    if (offset_ == b.frames) {
      offset_ = 0;
      ++tail;
      tail_.store(tail, std::memory_order_release);
    }
  }
  consumed_.fetch_add(got, std::memory_order_relaxed);
  return got;
}

void MixTap::clear() noexcept {
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  std::uint64_t tail = tail_.load(std::memory_order_relaxed);
  std::size_t frames = 0;
  for (; tail != head; ++tail) {
    frames += ring_[tail & (blocks_ - 1)].frames - offset_;
    offset_ = 0;
  }
  tail_.store(tail, std::memory_order_release);
  consumed_.fetch_add(frames, std::memory_order_relaxed);
}

std::uint64_t MixTap::position() const noexcept {
  return position_.load(std::memory_order_acquire);
}

std::size_t MixTap::buffered_frames() const noexcept {
  const std::uint64_t kept = kept_.load(std::memory_order_relaxed);
  const std::uint64_t used = consumed_.load(std::memory_order_relaxed);
  return kept > used ? static_cast<std::size_t>(kept - used) : 0;
}

std::uint64_t MixTap::dropped_frames() const noexcept {
  return dropped_.load(std::memory_order_relaxed);
}

} // namespace audio
