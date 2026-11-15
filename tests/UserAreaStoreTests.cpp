/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The World Builder's user areas: kept atomically with verified versions,
// drafts apart from admitted areas, an index of stable ids, undoable index
// changes, Duplicate layout with every id renamed, and saves that keep
// playing their own copy of an area whatever became of the store.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "core/character/Character.h"
#include "core/character/Player.h"
#include "core/content/LocationReader.h"
#include "core/content/Roster.h"
#include "core/init/NewGameConfig.h"
#include "core/save/AreaLayout.h"
#include "core/save/SaveGameManager.h"
#include "core/save/UserAreaStore.h"
#include "core/state/GameState.h"
#include "core/util/GameContext.h"
#include "engine/Random.h"
#include "resources/util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;
using save::AreaState;
using save::SaveGameManager;
using save::UserAreaStore;
using test_support::TempDir;

namespace {

constexpr std::string_view test_namespace = "u-0000abcd";

fs::path village_file() {
  return fs::path(__FILE__).parent_path().parent_path() / "data" /
         "locations" / "north-pole" / "santas-pizza-village.json";
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

json village() { return json::parse(read_file(village_file())); }

// A new area in the village's style, as Duplicate layout makes it.
json new_area(std::string_view slug, std::string_view name) {
  const std::string key = std::string(test_namespace) + ":" + std::string(slug);
  const save::DuplicatedLayout copy = save::duplicate_layout(
      village(), key, std::string(test_namespace) + "." + std::string(slug),
      name);
  INFO(core::content::describe(copy.findings));
  REQUIRE(copy.ok);
  return copy.document;
}

// Every entity id of a document.
std::set<std::string> ids_of(const json& doc) {
  std::set<std::string> ids;
  const auto add = [&ids](const json& list) {
    for (const json& e : list) {
      ids.insert(e.at("id").get<std::string>());
    }
  };
  add(doc.at("districts"));
  add(doc.at("properties"));
  add(doc.at("food_suppliers"));
  for (const auto& [role, value] : doc.at("actors").items()) {
    if (value.is_array()) {
      add(value);
    }
    else if (value.is_object()) {
      ids.insert(value.at("id").get<std::string>());
    }
  }
  add(doc.at("assets").at("attachments"));
  return ids;
}

std::shared_ptr<const core::content::LocationDefinition>
definition_of(const json& doc) {
  auto read = core::content::read_location(doc);
  REQUIRE(read.definition);
  return read.definition;
}

} // namespace

TEST_CASE("Duplicate layout renames every id and keeps every reference",
          "[areas][store]") {
  const json original = village();
  const json copy = new_area("north-pole-copy", "Frost Hollow");

  // No id is shared with the original, and none twice in the copy.
  const std::set<std::string> before = ids_of(original);
  const std::set<std::string> after = ids_of(copy);
  CHECK(after.size() == before.size());
  for (const std::string& id : after) {
    INFO(id);
    CHECK_FALSE(before.contains(id));
  }

  // Every premise's realtor is one of the copy's own realtors.
  std::set<std::string> realtors;
  for (const json& r : copy.at("actors").at("realtors")) {
    realtors.insert(r.at("id").get<std::string>());
  }
  for (const json& p : copy.at("properties")) {
    CHECK(realtors.contains(p.at("realtor").get<std::string>()));
  }

  // A new area of its own, design only, to admit.
  CHECK(copy.at("location_key") == "u-0000abcd:north-pole-copy");
  CHECK(copy.at("provenance").at("project_id") == "u-0000abcd.north-pole-copy");
  CHECK(copy.at("display").at("name") == "Frost Hollow");
  CHECK(copy.at("revision") == 1);
  CHECK_FALSE(copy.contains("integrity"));
  // The layout is the same: tiles, districts by index, premise places.
  CHECK(copy.at("tiles") == original.at("tiles"));
  CHECK(copy.at("district_cells") == original.at("district_cells"));
  CHECK(copy.at("properties").size() == original.at("properties").size());
}

TEST_CASE("The store keeps drafts and admitted areas apart, atomically",
          "[areas][store]") {
  TempDir dir;
  const std::string shipped = read_file(village_file());
  UserAreaStore store(dir.path());

  const std::string ns = store.installation_namespace();
  CHECK(ns.size() == 10);
  CHECK(ns.starts_with("u-"));
  CHECK(UserAreaStore(dir.path()).installation_namespace() == ns);

  const json area = new_area("north-pole-2", "Frost Hollow");
  const save::StoreResult kept = store.save_draft(area);
  INFO(kept.error);
  REQUIRE(kept.ok);
  CHECK(kept.id == "area-000001");

  auto entry = store.find(kept.id);
  REQUIRE(entry);
  CHECK(entry->state == AreaState::Draft);
  CHECK(entry->name == "Frost Hollow");
  CHECK(entry->territory == "north-pole");
  CHECK(entry->order == 0);
  CHECK(store.document(kept.id, AreaState::Draft) == area);
  // A draft never enters play.
  CHECK_FALSE(store.document(kept.id, AreaState::Admitted));
  CHECK(store.admitted("north-pole").empty());

  // A second area with the same key is refused.
  CHECK_FALSE(store.save_draft(area).ok);

  const save::StoreResult admitted = store.admit(kept.id);
  INFO(admitted.error);
  REQUIRE(admitted.ok);
  entry = store.find(kept.id);
  CHECK(entry->state == AreaState::Admitted);
  CHECK(entry->revision == 1);
  const auto playable = store.admitted("north-pole");
  REQUIRE(playable.size() == 1);
  CHECK(playable.front()->key.str() == "u-0000abcd:north-pole-2");
  CHECK(playable.front()->sha256 == entry->sha256);

  // Three verified versions are kept of each document.
  for (int i = 0; i < 4; ++i) {
    json edited = area;
    edited["display"]["short_name"] = "Hollow " + std::to_string(i);
    REQUIRE(store.save_draft(edited, kept.id).ok);
  }
  const auto drafts = save::SaveSlot(dir.path() / "areas" /
                                         "area-000001.draft.json",
                                     ".json")
                          .read_manifest();
  REQUIRE(drafts.manifest);
  CHECK(std::ranges::count(drafts.manifest->versions,
                           save::VersionStatus::Working,
                           &save::SaveVersion::status) == 3);

  // Nothing in the shipped data folder changed.
  CHECK(read_file(village_file()) == shipped);
}

TEST_CASE("A damaged version of an area falls back to the one before",
          "[areas][store]") {
  TempDir dir;
  UserAreaStore store(dir.path());
  const json first = new_area("north-pole-2", "Frost Hollow");
  const auto kept = store.save_draft(first);
  REQUIRE(kept.ok);
  json second = first;
  second["display"]["short_name"] = "Hollow";
  REQUIRE(store.save_draft(second, kept.id).ok);
  CHECK(store.document(kept.id, AreaState::Draft) == second);

  // The newest version and the slot's own copy are both damaged.
  const save::SaveSlot slot(dir.path() / "areas" / "area-000001.draft.json",
                            ".json");
  const auto manifest = slot.read_manifest().manifest;
  REQUIRE(manifest);
  for (const fs::path& file :
       {slot.version_path(*manifest->active), slot.file()}) {
    std::string bytes = read_file(file);
    bytes.resize(bytes.size() / 2);
    write_file(file, bytes);
  }
  CHECK(store.document(kept.id, AreaState::Draft) == first);
}

TEST_CASE("Admission, rename and reorder can be undone", "[areas][store]") {
  TempDir dir;
  UserAreaStore store(dir.path());
  const auto a = store.save_draft(new_area("north-pole-2", "Frost Hollow"));
  const auto b = store.save_draft(new_area("north-pole-3", "Elf Harbour"));
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(store.find(b.id)->order == 1);

  REQUIRE(store.reorder(b.id, 0).ok);
  CHECK(store.find(b.id)->order == 0);
  CHECK(store.find(a.id)->order == 1);
  REQUIRE(store.undo_last_change().ok);
  CHECK(store.find(b.id)->order == 1);

  REQUIRE(store.admit(a.id).ok);
  CHECK(store.find(a.id)->state == AreaState::Admitted);
  REQUIRE(store.undo_last_change().ok);
  CHECK(store.find(a.id)->state == AreaState::Draft);

  REQUIRE(store.admit(a.id).ok);
  REQUIRE(store.rename(a.id, "Snowdrift").ok);
  auto renamed = store.find(a.id);
  CHECK(renamed->name == "Snowdrift");
  // A rename is a new revision of the admitted document; its key stays.
  CHECK(renamed->revision == 2);
  CHECK(renamed->location_key == "u-0000abcd:north-pole-2");
  CHECK(store.document(a.id, AreaState::Admitted)->at("display").at("name") ==
        "Snowdrift");
  // One undo takes the whole rename back, the document it plays included.
  REQUIRE(store.undo_last_change().ok);
  CHECK(store.find(a.id)->name == "Frost Hollow");
  CHECK(store.find(a.id)->revision == 1);
  CHECK(store.document(a.id, AreaState::Admitted)->at("display").at("name") ==
        "Frost Hollow");
  CHECK(store.find(a.id)->id == a.id);
}

TEST_CASE("A save's own copy of an area outlives the store", "[areas][save]") {
  TempDir dir;
  const json area = new_area("north-pole-2", "Frost Hollow");
  {
    UserAreaStore store(dir.path());
    const auto kept = store.save_draft(area);
    REQUIRE(kept.ok);
    REQUIRE(store.admit(kept.id).ok);
  }
  // The copy a save embeds is the admitted document.
  UserAreaStore store(dir.path());
  const auto entry = store.find_by_key("u-0000abcd:north-pole-2");
  REQUIRE(entry);
  const auto embedded =
      definition_of(*store.document(entry->id, AreaState::Admitted));
  CHECK(save::area_differences({embedded}, store).empty());

  // Edited and admitted again: the save's copy now differs.
  REQUIRE(store.rename(entry->id, "Snowdrift").ok);
  auto differences = save::area_differences({embedded}, store);
  REQUIRE(differences.size() == 1);
  CHECK_FALSE(differences.front().missing);
  CHECK(differences.front().name == "Frost Hollow");

  // The store is gone: still told, and the copy can go back in.
  fs::remove_all(dir.path());
  UserAreaStore empty(dir.path());
  differences = save::area_differences({embedded}, empty);
  REQUIRE(differences.size() == 1);
  CHECK(differences.front().missing);
  const auto restored = empty.keep_copy(*embedded);
  INFO(restored.error);
  REQUIRE(restored.ok);
  CHECK(save::area_differences({embedded}, empty).empty());

  // Shipped areas are never in question.
  CHECK(save::area_differences({definition_of(village())}, empty).empty());
}

TEST_CASE("A Classic game is untouched by the area store", "[areas][save]") {
  core::character::CharacterData character{};
  GameState state;
  GameContext context;
  std::array<std::unique_ptr<Player>, NUMBER_OF_PLAYERS> players;
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    players[i] = std::make_unique<Player>(
        core::PlayerId{i}, core::character::Character(&character), i == 0);
  }
  state.players(std::move(players));
  state.active_player(core::PlayerId{0});
  engine::random::set_state(0x5EED1234);
  const auto before = SaveGameManager::fingerprint(state, context);

  TempDir dir;
  UserAreaStore store(dir.path());
  REQUIRE(store.save_draft(new_area("north-pole-2", "Frost Hollow")).ok);
  CHECK(SaveGameManager::area_differences(state, store).empty());
  CHECK(SaveGameManager::keep_area_copies(state, store) == 0);

  const auto after = SaveGameManager::fingerprint(state, context);
  CHECK(after.hash == before.hash);
  CHECK(after.document.at("version") == save::SAVE_FORMAT_VERSION);
  CHECK_FALSE(after.document.contains("scenario"));
}

TEST_CASE("A game in a user area loads from its save with the store gone",
          "[areas][save][game]") {
  const auto game_path = test_support::game_path();
  if (!game_path) {
    SKIP("Set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE");
  }
  fileutils::game_data_path(game_path->string());

  TempDir dir;
  const auto area = definition_of(new_area("north-pole-2", "Frost Hollow"));
  auto roster = core::content::Roster::from_entries({core::content::RosterEntry{
      .key = area->key,
      .source = core::content::RosterSource::Definition,
      .classic_number = std::nullopt,
      .definition = area}});
  REQUIRE(roster);

  GameContext context;
  GameState state;
  state.begin_initialization();
  auto& config = state.initializer();
  config.mode = GameMode::FREEMODE;
  config.difficulty = init::DifficultyLevel::NORMAL;
  config.roster = *roster;
  config.city_id = core::CityId{0};
  config.players.resize(NUMBER_OF_PLAYERS);
  for (int i = 0; i < NUMBER_OF_PLAYERS; ++i) {
    config.players[i].name = "Player " + std::to_string(i + 1);
    config.players[i].character_id = i;
    config.players[i].is_human = (i == 0);
  }
  state.finalize_initialization(context);
  const std::string kept = SaveGameManager::fingerprint(state, context).hash;

  const fs::path file = dir.path() / "frost.plsave";
  REQUIRE(SaveGameManager::save(state, context, file.string()));
  CHECK(SaveGameManager::get_save_version(file.string()) == 4);

  // No store anywhere: the game loads from the save's own copy, and says so.
  GameState loaded;
  std::string error;
  std::string notice;
  REQUIRE(SaveGameManager::load(loaded, context, file.string(), &error,
                                &notice));
  CHECK(SaveGameManager::fingerprint(loaded, context).hash == kept);
  CHECK(notice.find("its own copy of Frost Hollow") != std::string::npos);
  REQUIRE(loaded.roster().entries().size() == 1);
  CHECK(loaded.roster().entries().front().definition->sha256 == area->sha256);

  // The recovery path puts it back into a store.
  UserAreaStore store(dir.path() / "areas");
  CHECK(SaveGameManager::keep_area_copies(loaded, store) == 1);
  CHECK(SaveGameManager::area_differences(loaded, store).empty());
}
