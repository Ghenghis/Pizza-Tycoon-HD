/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The restaurant's pack-only sound cues (RestaurantSoundCues.h): doors,
// payment and plates on the walkers' moments, and the kitchen beds. They fire
// with the placement they are given, on frame time; with the original effects
// nothing reaches the audio at all; and they never touch the simulation's
// random state.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/Random.h"
#include "engine/audio/AudioManager.h"
#include "engine/geom/Spaces.h"
#include "views/base/ViewDefs.h"
#include "views/screens/restaurant/RestaurantSoundCues.h"

using views::screens::restaurant::AudioCueSink;
using views::screens::restaurant::CueSink;
using views::screens::restaurant::kitchen_bed_for;
using views::screens::restaurant::pack_event_id;
using views::screens::restaurant::PackCue;
using views::screens::restaurant::panel_placement;
using views::screens::restaurant::RestaurantSoundCues;

namespace {

struct Heard {
  enum class Kind { Play, Loop, Stop } kind;
  PackCue cue;
  audio::Placement place;
  uint64_t at_ms;
};

class RecordingSink final : public CueSink {
public:
  uint64_t now_ms = 0;
  std::vector<Heard> heard;
  void play(PackCue cue, audio::Placement place) override {
    heard.push_back({Heard::Kind::Play, cue, place, now_ms});
  }
  void loop(PackCue cue) override {
    heard.push_back({Heard::Kind::Loop, cue, {}, now_ms});
  }
  void stop(PackCue cue) override {
    heard.push_back({Heard::Kind::Stop, cue, {}, now_ms});
  }
};

bool same_place(audio::Placement a, audio::Placement b) {
  return a.pan == b.pan && a.gain == b.gain;
}

// Advances both clocks by 1 ms at a time.
void run_for(RestaurantSoundCues& cues, RecordingSink& sink, uint32_t ms) {
  for (uint32_t i = 0; i < ms; ++i) {
    ++sink.now_ms;
    cues.advance(1);
  }
}

const audio::Placement door{-0.6f, 1.0f};
const audio::Placement table{0.4f, 0.8f};

} // namespace

TEST_CASE("Restaurant cues: the pack events have their own ids",
          "[audio][cues]") {
  std::set<std::string> ids;
  for (int i = 0; i < static_cast<int>(PackCue::Count); ++i) {
    const auto id = pack_event_id(static_cast<PackCue>(i));
    CHECK_FALSE(id.empty());
    ids.emplace(id);
  }
  CHECK(ids.size() == static_cast<std::size_t>(PackCue::Count));
  CHECK(pack_event_id(PackCue::GuestEnter) == "guest-enter");
  CHECK(pack_event_id(PackCue::GuestLeave) == "guest-leave");
  CHECK(pack_event_id(PackCue::DoorClose) == "door-close");
  CHECK(pack_event_id(PackCue::Payment) == "payment");
  CHECK(pack_event_id(PackCue::PlateServed) == "plate-served");
  CHECK(pack_event_id(PackCue::KitchenBusy) == "kitchen-busy");
  CHECK(pack_event_id(PackCue::KitchenIdle) == "kitchen-idle");
}

TEST_CASE("Restaurant cues: a guest coming in opens and then closes the door",
          "[audio][cues]") {
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    INFO("seed " << seed);
    RecordingSink sink;
    RestaurantSoundCues cues(sink, seed);
    cues.guest_entered(door);
    REQUIRE(sink.heard.size() == 1);
    CHECK(sink.heard[0].kind == Heard::Kind::Play);
    CHECK(sink.heard[0].cue == PackCue::GuestEnter);
    CHECK(same_place(sink.heard[0].place, door));

    run_for(cues, sink, RestaurantSoundCues::door_close_min_ms - 1);
    CHECK(sink.heard.size() == 1); // not before 0.6 s
    run_for(cues, sink,
            RestaurantSoundCues::door_close_max_ms -
                RestaurantSoundCues::door_close_min_ms + 1);
    REQUIRE(sink.heard.size() == 2); // by 1.2 s
    CHECK(sink.heard[1].cue == PackCue::DoorClose);
    CHECK(same_place(sink.heard[1].place, door));
    CHECK(sink.heard[1].at_ms >= RestaurantSoundCues::door_close_min_ms);
    CHECK(sink.heard[1].at_ms <= RestaurantSoundCues::door_close_max_ms);
    CHECK(cues.pending() == 0);
  }
}

TEST_CASE("Restaurant cues: a guest leaving pays at the table and goes out "
          "through the door",
          "[audio][cues]") {
  RecordingSink sink;
  RestaurantSoundCues cues(sink, 7);
  cues.guest_leaving(door, table);
  REQUIRE(sink.heard.size() == 2);
  CHECK(sink.heard[0].cue == PackCue::Payment);
  CHECK(same_place(sink.heard[0].place, table));
  CHECK(sink.heard[1].cue == PackCue::GuestLeave);
  CHECK(same_place(sink.heard[1].place, door));
  run_for(cues, sink, RestaurantSoundCues::door_close_max_ms);
  REQUIRE(sink.heard.size() == 3);
  CHECK(sink.heard[2].cue == PackCue::DoorClose);
  CHECK(same_place(sink.heard[2].place, door));
}

TEST_CASE("Restaurant cues: a served pizza puts a plate down at the table",
          "[audio][cues]") {
  RecordingSink sink;
  RestaurantSoundCues cues(sink, 7);
  cues.pizza_served(table);
  REQUIRE(sink.heard.size() == 1);
  CHECK(sink.heard[0].cue == PackCue::PlateServed);
  CHECK(same_place(sink.heard[0].place, table));
  run_for(cues, sink, 5000);
  CHECK(sink.heard.size() == 1);
}

TEST_CASE("Restaurant cues: a cue waits 1.5 s before it plays again",
          "[audio][cues]") {
  RecordingSink sink;
  RestaurantSoundCues cues(sink, 3);
  cues.pizza_served(table);
  run_for(cues, sink, RestaurantSoundCues::min_gap_ms - 1);
  cues.pizza_served(table);
  CHECK(sink.heard.size() == 1);
  run_for(cues, sink, 1);
  cues.pizza_served(table);
  CHECK(sink.heard.size() == 2);

  // A door that does not open does not close either.
  RecordingSink doors;
  RestaurantSoundCues door_cues(doors, 3);
  door_cues.guest_entered(door);
  run_for(door_cues, doors, 100);
  door_cues.guest_entered(door);
  run_for(door_cues, doors, 3000);
  int opens = 0;
  int closes = 0;
  for (const auto& h : doors.heard) {
    opens += h.cue == PackCue::GuestEnter ? 1 : 0;
    closes += h.cue == PackCue::DoorClose ? 1 : 0;
  }
  CHECK(opens == 1);
  CHECK(closes == 1);
}

TEST_CASE("Restaurant cues: the kitchen bed follows the kitchen view",
          "[audio][cues][kitchen]") {
  CHECK(kitchen_bed_for(true, true, true, 22) == PackCue::KitchenBusy);
  CHECK(kitchen_bed_for(true, true, true, 21) == PackCue::KitchenIdle);
  CHECK(kitchen_bed_for(true, true, false, 90) == PackCue::KitchenIdle);
  CHECK(kitchen_bed_for(true, false, true, 90) == std::nullopt);
  CHECK(kitchen_bed_for(false, true, true, 90) == std::nullopt);

  RecordingSink sink;
  RestaurantSoundCues cues(sink, 1);
  cues.kitchen_bed(PackCue::KitchenBusy); // the kitchen view opens
  cues.kitchen_bed(PackCue::KitchenBusy); // nothing changed
  REQUIRE(sink.heard.size() == 1);
  CHECK(sink.heard[0].kind == Heard::Kind::Loop);
  CHECK(sink.heard[0].cue == PackCue::KitchenBusy);

  cues.kitchen_bed(PackCue::KitchenIdle); // the crowd thins out
  REQUIRE(sink.heard.size() == 3);
  CHECK(sink.heard[1].kind == Heard::Kind::Stop);
  CHECK(sink.heard[1].cue == PackCue::KitchenBusy);
  CHECK(sink.heard[2].kind == Heard::Kind::Loop);
  CHECK(sink.heard[2].cue == PackCue::KitchenIdle);

  cues.kitchen_bed(std::nullopt); // back to the dining room
  REQUIRE(sink.heard.size() == 4);
  CHECK(sink.heard[3].kind == Heard::Kind::Stop);
  CHECK(sink.heard[3].cue == PackCue::KitchenIdle);

  // Closing the view stops the bed and drops a door still to close.
  cues.kitchen_bed(PackCue::KitchenBusy);
  cues.guest_entered(door);
  cues.stop_all();
  CHECK(cues.kitchen_bed() == std::nullopt);
  CHECK(cues.pending() == 0);
  CHECK(sink.heard.back().kind == Heard::Kind::Stop);
  const auto before = sink.heard.size();
  run_for(cues, sink, 2000);
  CHECK(sink.heard.size() == before);
}

TEST_CASE("Restaurant cues: placement on the restaurant panel",
          "[audio][cues]") {
  using views::base::MAIN_PANEL_HEIGHT;
  using views::base::MAIN_PANEL_WIDTH;
  const auto left = panel_placement(ViewPoint{0, MAIN_PANEL_HEIGHT / 2});
  const auto middle =
      panel_placement(ViewPoint{MAIN_PANEL_WIDTH / 2, MAIN_PANEL_HEIGHT / 2});
  const auto right =
      panel_placement(ViewPoint{MAIN_PANEL_WIDTH, MAIN_PANEL_HEIGHT / 2});
  CHECK(left.pan < 0.0f);
  CHECK(middle.pan == Catch::Approx(0.0f).margin(0.01));
  CHECK(right.pan > 0.0f);
  const auto expected = audio::screen_placement(
      10.0f, 20.0f, static_cast<float>(MAIN_PANEL_WIDTH),
      static_cast<float>(MAIN_PANEL_HEIGHT));
  CHECK(same_place(panel_placement(ViewPoint{10, 20}), expected));
}

TEST_CASE("Restaurant cues: nothing reaches the audio with the original "
          "effects",
          "[audio][cues]") {
  audio::AudioManager am; // not initialised: no device is needed
  REQUIRE(am.effects() == audio::EffectsSet::ORIGINAL);
  AudioCueSink sink(&am);
  RestaurantSoundCues cues(sink, 9);
  cues.guest_entered(door);
  cues.guest_leaving(door, table);
  cues.pizza_served(table);
  cues.kitchen_bed(PackCue::KitchenBusy);
  cues.kitchen_bed(PackCue::KitchenIdle);
  for (int i = 0; i < 3000; ++i) {
    cues.advance(1);
  }
  cues.stop_all();
  CHECK(sink.sent() == 0);
  CHECK_FALSE(am.mixer().any_effect_playing());

  // No audio at all (headless runs).
  AudioCueSink none(nullptr);
  none.play(PackCue::GuestEnter, door);
  none.loop(PackCue::KitchenBusy);
  none.stop(PackCue::KitchenBusy);
  CHECK(none.sent() == 0);
}

TEST_CASE("Restaurant cues: the simulation's random state is untouched",
          "[audio][cues][golden]") {
  engine::random::seed(0x5EED);
  const auto before = engine::random::get_state();
  RecordingSink sink;
  RestaurantSoundCues cues(sink, 11);
  for (int i = 0; i < 50; ++i) {
    cues.guest_entered(door);
    cues.guest_leaving(door, table);
    cues.pizza_served(table);
    run_for(cues, sink, 1600);
  }
  CHECK(engine::random::get_state() == before);
  CHECK(sink.heard.size() > 100);
}
