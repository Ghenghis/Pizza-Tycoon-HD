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

namespace music {

// A single-producer, single-consumer ring of interleaved int16 stereo audio
// at the output rate, allocated once. The producer is the music worker; the
// consumer is the audio callback. Neither ever waits for the other: each
// owns one index, publishes it with a release store and reads the other's
// with an acquire load.
//
// The audio sits in blocks of up to BLOCK_FRAMES frames. Each block carries
// a tag: the playback generation it was made for, and which track and frame
// it starts at. The consumer plays only blocks of the generation it asks
// for. Older blocks are dropped unplayed, so audio decoded before a mode
// change, a skip or a pause can never be heard after it; a newer block
// waits.
class PcmRing {
public:
  static constexpr std::size_t CHANNELS = 2;
  static constexpr std::size_t BLOCK_FRAMES = 256;
  // 128 blocks: 2^15 frames, 0.74 s at 44100 Hz.
  static constexpr std::size_t DEFAULT_BLOCKS = 128;

  struct Tag {
    std::uint32_t generation = 0;
    std::uint32_t track = 0; // which track of the generation, as numbered
                             // by the producer
    std::uint64_t frame = 0; // of the first frame, within its track
    bool operator==(const Tag&) const = default;
  };

  // Holds blocks rounded up to a power of two (at least 2).
  explicit PcmRing(std::size_t blocks = DEFAULT_BLOCKS);
  PcmRing(const PcmRing&) = delete;
  PcmRing& operator=(const PcmRing&) = delete;

  // Producer. Copies whole frames from samples into free blocks, tagged with
  // tag (each block's frame counts on from the one before). Returns the
  // frames copied: fewer when the ring fills up.
  std::size_t write(std::span<const std::int16_t> samples, Tag tag) noexcept;
  // Producer. Frames that fit now, in whole free blocks.
  [[nodiscard]] std::size_t writable_frames() const noexcept;
  // Producer. The generation being produced for, 0 for none. A read of that
  // generation that comes up short counts as an underrun; for any other it
  // is silence the player expects (a pause, a stop, the gap of a switch).
  void producing(std::uint32_t generation) noexcept;

  // Consumer. Fills out with whole frames of generation, dropping older
  // blocks on the way, and the shortfall with silence. Returns the frames
  // read.
  std::size_t read(std::span<std::int16_t> out,
                   std::uint32_t generation) noexcept;
  // Consumer. Drops the blocks older than generation without playing
  // anything, so the producer gets the room back.
  void drop_stale(std::uint32_t generation) noexcept;

  // Any thread.
  [[nodiscard]] std::size_t capacity_frames() const noexcept {
    return blocks_ * BLOCK_FRAMES;
  }
  // Frames in the ring, of every generation.
  [[nodiscard]] std::size_t buffered_frames() const noexcept;
  // The tag of the frame after the last one read: where playback is, as
  // heard. Zero before anything was read.
  [[nodiscard]] Tag heard() const noexcept;
  [[nodiscard]] std::uint32_t underruns() const noexcept;
  [[nodiscard]] std::uint64_t frames_read() const noexcept;
  // Blocks dropped unplayed because their generation was over.
  [[nodiscard]] std::uint64_t blocks_dropped() const noexcept;

private:
  struct Block {
    Tag tag;
    std::uint32_t frames = 0;
    std::int16_t samples[BLOCK_FRAMES * CHANNELS];
  };

  // Consumer only: drops stale blocks from r on; returns the new r.
  std::size_t skip_stale(std::size_t r, std::size_t w,
                         std::uint32_t generation) noexcept;

  std::size_t blocks_;
  std::size_t mask_;
  std::unique_ptr<Block[]> ring_;

  alignas(64) std::atomic<std::size_t> write_{0}; // blocks published
  alignas(64) std::atomic<std::size_t> read_{0};  // blocks finished
  // Consumer only: frames of the block at read_ already played.
  std::uint32_t offset_ = 0;

  alignas(64) std::atomic<std::uint32_t> producing_{0};
  std::atomic<std::uint32_t> underruns_{0};
  std::atomic<std::uint64_t> frames_read_{0};
  std::atomic<std::uint64_t> blocks_dropped_{0};
  // heard(): generation and track packed in one word, the frame in another.
  // They are written together by the consumer and may be read a block
  // apart, which is fine for a position display.
  std::atomic<std::uint64_t> heard_id_{0};
  std::atomic<std::uint64_t> heard_frame_{0};
};

} // namespace music
