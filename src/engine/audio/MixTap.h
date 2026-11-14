/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace audio {

// The game's own final mix, for a recorder (docs/studio/notes/recording.md):
// a single-producer, single-consumer ring of interleaved int16 stereo,
// allocated once. The producer is the audio callback, which never waits,
// locks or allocates here; the consumer is one recorder thread.
//
// Every frame has a position: its number among all the frames the mixer has
// made, counted from 0, whether or not the tap was on. So a recorder times
// audio by sample position, never by the clock or the frame rate. The audio
// sits in blocks of up to BLOCK_FRAMES frames, each stamped with the
// position of its first frame. When the ring is full the callback drops the
// newest audio and counts it; the reader then sees a jump in positions of
// exactly the frames lost, never audio out of place.
class MixTap {
public:
  static constexpr std::size_t CHANNELS = 2;
  static constexpr std::size_t BLOCK_FRAMES = 256;
  // 512 blocks: 2^17 frames, 2.97 s at 44100 Hz (512 KiB).
  static constexpr std::size_t DEFAULT_BLOCKS = 512;

  // Holds blocks rounded up to a power of two (at least 2).
  explicit MixTap(std::size_t blocks = DEFAULT_BLOCKS);
  MixTap(const MixTap&) = delete;
  MixTap& operator=(const MixTap&) = delete;

  // Any thread. Off by default: the callback then only counts frames.
  void enabled(bool on) noexcept;
  [[nodiscard]] bool enabled() const noexcept;

  // Producer (the audio callback). The next frames of the mix: counted
  // always, kept while enabled and while there is room. Odd trailing
  // samples are ignored.
  void write(std::span<const std::int16_t> samples) noexcept;

  // Consumer. Copies the next run of frames with consecutive positions into
  // out (whole frames, at most out.size() / 2) and returns how many, 0 when
  // there are none. *first gets the position of the first one. A run ends
  // at a gap, so the next call starts after it.
  std::size_t read(std::span<std::int16_t> out,
                   std::uint64_t* first = nullptr) noexcept;
  // Consumer. Drops everything kept so far, for a fresh start.
  void clear() noexcept;

  // Any thread.
  [[nodiscard]] std::size_t capacity_frames() const noexcept {
    return blocks_ * BLOCK_FRAMES;
  }
  // Frames made by the mixer so far: the position of the next one.
  [[nodiscard]] std::uint64_t position() const noexcept;
  // Frames kept and not yet read.
  [[nodiscard]] std::size_t buffered_frames() const noexcept;
  // Frames that came while enabled but found no room.
  [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

private:
  struct Block {
    std::uint64_t first = 0; // position of its first frame
    std::size_t frames = 0;
    std::int16_t samples[BLOCK_FRAMES * CHANNELS];
  };

  std::size_t blocks_;
  std::unique_ptr<Block[]> ring_;
  std::atomic<std::uint64_t> head_{0}; // blocks written: producer's
  std::atomic<std::uint64_t> tail_{0}; // blocks read: consumer's
  std::size_t offset_ = 0;             // frames read of the tail block
  std::atomic<std::uint64_t> position_{0};
  std::atomic<std::uint64_t> kept_{0};     // frames kept: producer's
  std::atomic<std::uint64_t> consumed_{0}; // frames read or cleared
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<bool> enabled_{false};
};

} // namespace audio
