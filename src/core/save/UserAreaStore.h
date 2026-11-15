/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The areas a player makes in the World Builder, kept as location documents
// in the user content folder (base dir + "content/areas/"), never in the
// shipped data folder. Each area has a stable id ("area-000017"), its place
// in its territory's order and its name in an index. It has two documents
// kept apart: the draft being edited and the admitted one that games play.
// Every document, and the index, is written through util::AtomicFile with
// the last three verified versions kept, as save slots keep theirs (see
// SaveVersions.h); a damaged version falls back to the one before. Changes
// to the index (admission, rename, reorder, withdrawal) can be undone.
//
// One writer: the game thread. The catalog (AreaCatalog) reads list() and
// admitted(); the generator gets installation_namespace().

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/content/ContentKey.h"
#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"
#include "core/save/SaveVersions.h"

namespace save {

enum class AreaState { Draft, Admitted };

struct UserAreaEntry {
  std::string id;          // "area-000017": never reused or renumbered
  std::string location_key; // the document's key, "u-3f9a1c2e:..."
  std::string name;        // as the area list shows it
  std::string territory;   // world.placement: a region id or the bonus route
  int order = 0;           // presentation order within the territory
  AreaState state = AreaState::Draft;
  int revision = 0;        // of the admitted document; 0 before admission
  std::string sha256;      // of the admitted document; empty before
  // The version of the admitted document the area plays, so undoing an
  // index change brings back the document that went with it.
  std::uint64_t admitted_version = 0;
  std::string created_utc;
  std::string updated_utc;
};

struct StoreResult {
  bool ok = false;
  std::string id;      // the area written to, when ok
  std::string error;   // why not, for the player
  core::content::Findings findings; // validator findings, for admit
};

class UserAreaStore {
public:
  // The user content folder if root is empty.
  explicit UserAreaStore(std::filesystem::path root = {});

  [[nodiscard]] static std::filesystem::path default_root();
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  // "u-" and eight hex digits, made once for this installation and kept, so
  // keys made on two machines never collide (location-contract.md 2.1).
  [[nodiscard]] std::string installation_namespace() const;

  // Every area, by territory and then presentation order.
  [[nodiscard]] std::vector<UserAreaEntry> list() const;
  [[nodiscard]] std::optional<UserAreaEntry> find(std::string_view id) const;
  [[nodiscard]] std::optional<UserAreaEntry>
  find_by_key(std::string_view location_key) const;

  // Keeps a draft: a new area without an id, a new version of an area's
  // draft with one. It must be a location document with a key no other area
  // has; it need not pass the validator yet. A draft never enters play.
  StoreResult save_draft(const nlohmann::json& document,
                         std::optional<std::string> id = {});

  // Makes the draft the area's admitted document, once it passes
  // read_location and validate_location: its integrity hash is written and,
  // if it differs from the one admitted before, its revision goes up by one.
  StoreResult admit(std::string_view id);

  // Index changes; each can be undone.
  StoreResult rename(std::string_view id, std::string_view name);
  // Moves the area to position new_order (from 0) in its territory.
  StoreResult reorder(std::string_view id, int new_order);
  // Takes the area out of play; its documents stay.
  StoreResult withdraw(std::string_view id);
  // Puts the index back as it was before its last change. Documents are
  // never changed or removed by an undo.
  StoreResult undo_last_change();

  // The newest verified version of an area's draft or admitted document.
  [[nodiscard]] std::optional<nlohmann::json>
  document(std::string_view id, AreaState which) const;

  // The admitted areas of a territory, read and validated, in order.
  [[nodiscard]] std::vector<std::shared_ptr<const core::content::LocationDefinition>>
  admitted(std::string_view territory) const;

  // A save's own copy of an area, back into the store: a new admitted area
  // if no area has its key, else a new draft of that area, so the player's
  // own edits are never overwritten.
  StoreResult keep_copy(const core::content::LocationDefinition& definition);

private:
  [[nodiscard]] SaveSlot index_slot() const;
  [[nodiscard]] SaveSlot area_slot(std::string_view id, AreaState which) const;

  struct Index {
    std::uint64_t next_id = 1;
    std::vector<UserAreaEntry> areas;
  };
  [[nodiscard]] Index read_index() const;
  StoreResult write_index(const Index& index, std::string_view made_for);
  // Writes a new draft version; the index is the caller's to write.
  [[nodiscard]] std::optional<std::string>
  write_draft(std::string_view id, const nlohmann::json& document);
  // Admits the area's draft into index, without writing the index.
  StoreResult admit_into(Index& index, std::string_view id);

  std::filesystem::path root_;
};

// How a save's areas stand against the store, for the notice after a load.
struct AreaDifference {
  std::string location_key;
  std::string name;
  bool missing = false; // the store has no admitted document for it
};

// The user areas in a game's roster whose admitted document in the store is
// gone or differs from the save's own copy. The game always plays the copy.
[[nodiscard]] std::vector<AreaDifference>
area_differences(const std::vector<std::shared_ptr<const core::content::LocationDefinition>>& embedded,
                 const UserAreaStore& store);

} // namespace save
