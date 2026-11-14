/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/restaurant/RestaurantSoundCues.h"

#include <algorithm>
#include <cstddef>

#include "views/base/ViewDefs.h"

namespace views::screens::restaurant {

std::string_view pack_event_id(PackCue cue) {
  switch (cue) {
  case PackCue::GuestEnter:
    return "guest-enter";
  case PackCue::GuestLeave:
    return "guest-leave";
  case PackCue::DoorClose:
    return "door-close";
  case PackCue::Payment:
    return "payment";
  case PackCue::PlateServed:
    return "plate-served";
  case PackCue::KitchenBusy:
    return "kitchen-busy";
  case PackCue::KitchenIdle:
    return "kitchen-idle";
  case PackCue::Count:
    break;
  }
  return {};
}

bool AudioCueSink::enhanced() const {
  return audio_ && audio_->effects() == audio::EffectsSet::ENHANCED;
}

void AudioCueSink::play(PackCue cue, audio::Placement place) {
  if (enhanced()) {
    audio_->play_event(pack_event_id(cue), place);
    ++sent_;
  }
}

void AudioCueSink::loop(PackCue cue) {
  if (enhanced()) {
    looping_[static_cast<std::size_t>(cue)] =
        audio_->start_event(pack_event_id(cue));
    ++sent_;
  }
}

void AudioCueSink::stop(PackCue cue) {
  // Only what this sink started; stopped even if the effects were switched
  // since.
  auto& started = looping_[static_cast<std::size_t>(cue)];
  if (started && audio_) {
    audio_->stop_event(pack_event_id(cue));
    ++sent_;
  }
  started = false;
}

std::optional<PackCue> kitchen_bed_for(bool restaurant_fx, bool kitchen_view,
                                       bool open, int utilization) {
  if (!restaurant_fx || !kitchen_view) {
    return std::nullopt;
  }
  return open && utilization > 21 ? PackCue::KitchenBusy : PackCue::KitchenIdle;
}

audio::Placement panel_placement(ViewPoint p) {
  return audio::screen_placement(
      static_cast<float>(p.x), static_cast<float>(p.y),
      static_cast<float>(views::base::MAIN_PANEL_WIDTH),
      static_cast<float>(views::base::MAIN_PANEL_HEIGHT));
}

RestaurantSoundCues::RestaurantSoundCues(CueSink& sink, uint64_t seed)
    : sink_(sink), random_(seed) {}

bool RestaurantSoundCues::try_play(PackCue cue, audio::Placement place) {
  auto& last = last_played_[static_cast<std::size_t>(cue)];
  if (last && now_ms_ - *last < min_gap_ms) {
    return false;
  }
  last = now_ms_;
  sink_.play(cue, place);
  return true;
}

void RestaurantSoundCues::door_pair(PackCue opening, audio::Placement door) {
  if (!try_play(opening, door)) {
    return; // no door opened, so none closes
  }
  const uint32_t delay = random_.between(door_close_min_ms, door_close_max_ms);
  pending_.push_back(Pending{
      .due_ms = now_ms_ + delay, .cue = PackCue::DoorClose, .place = door});
}

void RestaurantSoundCues::guest_entered(audio::Placement door) {
  door_pair(PackCue::GuestEnter, door);
}

void RestaurantSoundCues::guest_leaving(audio::Placement door,
                                        audio::Placement table) {
  try_play(PackCue::Payment, table);
  door_pair(PackCue::GuestLeave, door);
}

void RestaurantSoundCues::pizza_served(audio::Placement table) {
  try_play(PackCue::PlateServed, table);
}

void RestaurantSoundCues::advance(uint32_t dt_ms) {
  now_ms_ += dt_ms;
  // In the order they were asked for.
  std::vector<Pending> due;
  std::erase_if(pending_, [&](const Pending& p) {
    if (p.due_ms <= now_ms_) {
      due.push_back(p);
      return true;
    }
    return false;
  });
  for (const Pending& p : due) {
    // A closing door is part of its pair: it is not held back by the gap.
    last_played_[static_cast<std::size_t>(p.cue)] = now_ms_;
    sink_.play(p.cue, p.place);
  }
}

void RestaurantSoundCues::kitchen_bed(std::optional<PackCue> bed) {
  if (bed == bed_) {
    return;
  }
  if (bed_) {
    sink_.stop(*bed_);
  }
  bed_ = bed;
  if (bed_) {
    sink_.loop(*bed_);
  }
}

void RestaurantSoundCues::stop_all() {
  pending_.clear();
  kitchen_bed(std::nullopt);
}

} // namespace views::screens::restaurant
