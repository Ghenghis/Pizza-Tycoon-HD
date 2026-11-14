/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/audio/AudioManager.h"
#include "engine/geom/Spaces.h"

// Cosmetic sounds of the restaurant that only an enhanced effects pack has
// (docs/studio/notes/sfx-matrix.md, "Event-bound one-shots" and the kitchen
// beds): doors as guests come and go, payment, a plate when a pizza is
// served, and the kitchen's bed. The 1994 game has none of them, so with the
// original effects nothing is ever asked of the audio: Classic sounds exactly
// as before.
//
// Purely cosmetic: the triggers are the views' own walkers, the delays run
// on the frame clock and the choices on a cosmetic generator, never the
// simulation's random streams, so the game is the same with or without them.

namespace views::screens::restaurant {

// The pack events these cues play (audio::PackEvent, by id). Pack-only: no
// original sound has them. DoorClose is proposed, not yet a pack event: until
// the platform adds it, it is silent like any id the effects do not know.
enum class PackCue : uint8_t {
  GuestEnter,
  GuestLeave,
  DoorClose,
  Payment,
  PlateServed,
  KitchenBusy,
  KitchenIdle,
  Count
};

// The event id an effects pack names the cue by ("guest-enter").
[[nodiscard]] std::string_view pack_event_id(PackCue cue);

// Where the cues go. The game's sink is AudioCueSink; tests record.
class CueSink {
public:
  virtual ~CueSink() = default;
  virtual void play(PackCue cue, audio::Placement place) = 0;
  virtual void loop(PackCue cue) = 0;
  virtual void stop(PackCue cue) = 0;
};

// Plays the cues through the AudioManager's pack events (play_event,
// start_event, stop_event): only while the enhanced effects are selected, so
// with the original effects nothing is sent at all; the AudioManager is
// silent too for an event the pack does not have.
class AudioCueSink final : public CueSink {
public:
  explicit AudioCueSink(audio::AudioManager* audio) : audio_(audio) {}

  void play(PackCue cue, audio::Placement place) override;
  void loop(PackCue cue) override;
  void stop(PackCue cue) override;

  // Requests handed to the AudioManager so far (tests).
  [[nodiscard]] int sent() const { return sent_; }

private:
  [[nodiscard]] bool enhanced() const;

  audio::AudioManager* audio_;
  std::array<bool, static_cast<std::size_t>(PackCue::Count)> looping_{};
  int sent_{0};
};

// Which kitchen bed plays: none without restaurant sounds or outside the
// kitchen view, busy while open with seating utilization above 21 % (the
// threshold of the dining room's first crowd level), idle otherwise.
[[nodiscard]] std::optional<PackCue> kitchen_bed_for(bool restaurant_fx,
                                                     bool kitchen_view,
                                                     bool open,
                                                     int utilization);

// The placement of a sound at p on the restaurant panel (game pixels).
[[nodiscard]] audio::Placement panel_placement(ViewPoint p);

class RestaurantSoundCues {
public:
  // A door closes this long after a guest came in or went out (sfx-matrix:
  // 0.6-1.2 s).
  static constexpr uint32_t door_close_min_ms = 600;
  static constexpr uint32_t door_close_max_ms = 1200;
  // A cue does not play again sooner than this (sfx-matrix: 1.5 s per pool).
  static constexpr uint32_t min_gap_ms = 1500;

  RestaurantSoundCues(CueSink& sink, uint64_t seed);

  // A guest comes in through the door: guest-enter (the door opening), then
  // door-close.
  void guest_entered(audio::Placement door);
  // A guest gets up to leave: payment at the table, guest-leave at the door,
  // then door-close.
  void guest_leaving(audio::Placement door, audio::Placement table);
  // A waiter puts a pizza down.
  void pizza_served(audio::Placement table);

  // Frame time: plays the door-closes that are due.
  void advance(uint32_t dt_ms);

  // Starts, swaps or stops the kitchen bed.
  void kitchen_bed(std::optional<PackCue> bed);
  [[nodiscard]] std::optional<PackCue> kitchen_bed() const { return bed_; }

  // Stops the bed and drops what is waiting (the view closes).
  void stop_all();

  [[nodiscard]] std::size_t pending() const { return pending_.size(); }

private:
  // Plays cue unless it played less than min_gap_ms ago.
  bool try_play(PackCue cue, audio::Placement place);
  void door_pair(PackCue opening, audio::Placement door);

  struct Pending {
    uint64_t due_ms;
    PackCue cue;
    audio::Placement place;
  };

  CueSink& sink_;
  audio::CosmeticRandom random_;
  uint64_t now_ms_{0};
  std::array<std::optional<uint64_t>, static_cast<std::size_t>(PackCue::Count)>
      last_played_{};
  std::vector<Pending> pending_;
  std::optional<PackCue> bed_;
};

} // namespace views::screens::restaurant
