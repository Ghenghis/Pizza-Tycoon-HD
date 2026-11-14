/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/music/MusicTypes.h"

namespace music {

// What the music owner tells its worker. Every command says where to be,
// never how far to move, so a newer one of a kind can stand in for an older
// one without changing the outcome.
struct Command {
  enum class Kind : std::uint8_t {
    QUEUE, // the tracks to play from now on, and the repeat mode
    PLAY,  // produce for generation from track index at frame
    HALT,  // stop producing; generation is the new, silent one
  };
  Kind kind = Kind::HALT;
  std::uint32_t generation = 0;    // PLAY, HALT
  std::vector<QueuedTrack> tracks; // QUEUE
  Repeat repeat = Repeat::OFF;     // QUEUE
  // QUEUE: the playing track's place in the new list, so it plays on
  // unbroken; NO_INDEX to leave playback to the next PLAY.
  std::size_t keep_index = NO_INDEX;
  std::size_t index = 0;   // PLAY
  std::uint64_t frame = 0; // PLAY: output frames into the track

  static constexpr std::size_t NO_INDEX = static_cast<std::size_t>(-1);
};

// The owner's commands on their way to the worker: bounded, and a push
// never fails or waits. When the queue is full, a new command replaces the
// newest queued one of its kind; when there is none of its kind, the queue
// first shrinks to the newest command of each kind. Not thread-safe by
// itself: MusicPlayback guards it with its lock.
class CommandQueue {
public:
  static constexpr std::size_t CAPACITY = 64;

  void push(Command command);
  [[nodiscard]] std::vector<Command> take();
  [[nodiscard]] std::size_t size() const { return items_.size(); }
  [[nodiscard]] bool empty() const { return items_.empty(); }
  [[nodiscard]] const std::vector<Command>& items() const { return items_; }
  // Pushes that replaced a queued command instead of adding one.
  [[nodiscard]] std::uint64_t coalesced() const { return coalesced_; }

private:
  std::vector<Command> items_;
  std::uint64_t coalesced_ = 0;
};

} // namespace music
