/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// What a worker may share with the game thread while it builds a save apart
// from the game: the random streams are per thread, and the factories' tables
// are filled once and then only read. Run these under ThreadSanitizer too.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "core/character/Character.h"
#include "core/character/Training.h"
#include "core/factories/CharacterFactory.h"
#include "core/factories/FloorPatternFactory.h"
#include "core/factories/FurnitureFactory.h"
#include "core/factories/IngredientFactory.h"
#include "core/factories/SupplierFactory.h"
#include "core/factories/TrainingFactory.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace {

// The next n draws of the calling thread's simulation stream.
std::vector<int> draws(int n) {
  std::vector<int> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    out.push_back(engine::random::rand_max_n(1000));
  }
  return out;
}

} // namespace

TEST_CASE("A worker's draws never move the game's random stream",
          "[random][thread]") {
  engine::random::seed(19950304);
  const std::uint32_t start = engine::random::get_state();
  const std::vector<int> expected = draws(200);
  engine::random::set_state(start);

  // A worker sets, draws from and seeds its own stream, as a build does.
  std::vector<int> worker_draws;
  std::uint32_t worker_start = 0;
  std::thread worker([&] {
    worker_start = engine::random::get_state();
    engine::random::set_state(start);
    worker_draws = draws(5000);
    engine::random::seed(7);
    static_cast<void>(draws(5000));
  });
  // Meanwhile the game thread draws as usual.
  const std::vector<int> game_draws = draws(200);
  worker.join();

  CHECK(game_draws == expected);
  // A new thread starts where the program does, not where the game is.
  CHECK(worker_start == 1);
  // From the same state, the worker drew the game's very sequence.
  CHECK(std::vector<int>(worker_draws.begin(), worker_draws.begin() + 200) ==
        expected);
  // And the game thread goes on from its own state alone.
  engine::random::set_state(start);
  CHECK(draws(200) == expected);
}

TEST_CASE("Every thread gets the same factory objects, made once",
          "[factories][thread][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  struct Seen {
    std::vector<const void*> pointers;
  };
  constexpr int threads = 8;
  std::array<Seen, threads> seen;
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&seen, t] {
      Seen& mine = seen[static_cast<std::size_t>(t)];
      // In a different order on each thread, so the first fills race.
      for (int i = 0; i < 8; ++i) {
        const int id = (i + t) % 8;
        mine.pointers.push_back(CharacterFactory::create_character(id));
        mine.pointers.push_back(CharacterFactory::create_character(100 + id));
      }
      mine.pointers.push_back(IngredientFactory::create_ingredient(0));
      for (const Supplier* s : SupplierFactory::all_suppliers()) {
        mine.pointers.push_back(s);
      }
      for (const Furniture* f : FurnitureFactory::all_furniture()) {
        mine.pointers.push_back(f);
      }
      mine.pointers.push_back(
          &FloorPatternFactory::create(static_cast<SupplierID>(0)));
      static_cast<void>(TrainingFactory::all_trainings());
    });
  }
  for (auto& thread : pool) {
    thread.join();
  }

  // Characters come in a per-thread order; compare them as sets.
  const auto as_set = [](const Seen& s) {
    return std::set<const void*>(s.pointers.begin(), s.pointers.end());
  };
  for (int t = 1; t < threads; ++t) {
    INFO("thread " << t);
    CHECK(as_set(seen[static_cast<std::size_t>(t)]) == as_set(seen[0]));
  }
  // Asked again, the same objects.
  CHECK(CharacterFactory::create_character(3) ==
        CharacterFactory::create_character(3));
}
