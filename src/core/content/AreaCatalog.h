/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The territories' area lists (WORLD-BUILDER-EXPANSION.md): each territory
// is its home location (page 1, Santa's Pizza Village for the North Pole)
// followed by the areas added to it, in a stable order. Page numbers are
// presentation only; an area is known by its id ("area-000001") and its
// location key, which never change, also through a rename.
//
// The catalog owns the creation path: Add Area generates a document
// (AreaGenerator.h), an Automatic one is admitted at once (it passed the
// validator), an empty draft stays a draft with its missing requirements. A
// failed generation changes nothing. Persistence is the user-area store's
// (src/core/save/UserAreaStore, the persistence lane), plugged in through
// AreaStoreHooks; without hooks the catalog lives in memory (tests, scripts).
//
// Games are not changed here: an established game's roster stays pinned
// until an explicit expansion (GameState::expand_roster, `nav area`).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/content/AreaGenerator.h"
#include "core/content/ContentKey.h"
#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"

namespace core::content {

enum class AreaState : uint8_t { Draft, Admitted };

[[nodiscard]] std::string_view area_state_name(AreaState state);

// The territory of a location: its region id, or its bonus route.
[[nodiscard]] std::string territory_of(const LocationDefinition& definition);

struct Area {
  std::string id; // "area-000001": stable, never reused
  std::string territory;
  std::string name;
  ContentKey key = *ContentKey::parse("pthd:unnamed");
  std::string document_id; // provenance.project_id
  AreaState state{AreaState::Draft};
  AreaMode mode{AreaMode::Automatic};
  std::uint64_t seed{0};
  int order{0}; // within the territory, after the home location
  nlohmann::json document;
  // Set once admitted: what a game builds the area from.
  std::shared_ptr<const LocationDefinition> definition;
};

// One page of a territory: the home location (no area id) or an area.
struct AreaPage {
  int page{0}; // 1-based, presentation only
  std::string area_id;
  ContentKey key = *ContentKey::parse("pthd:unnamed");
  std::string name;
  AreaState state{AreaState::Admitted};
};

// The user-area store's side (persistence lane). Every member is optional;
// one that is missing is done in memory only.
struct AreaStoreHooks {
  // A new area (no id) or a new version of one; the store's area id, or
  // nullopt with error.
  std::function<std::optional<std::string>(const nlohmann::json& document,
                                           std::optional<std::string> id,
                                           std::string* error)>
      save_draft;
  // Admits a stored draft into play; false with the findings that stop it.
  std::function<bool(const std::string& id, Findings* findings)> admit;
  // The installation's key namespace ("u-3f9a1c2e").
  std::function<std::string()> installation_namespace;
  // A rename the store does in one step (one undo): display.name only, one
  // revision on, integrity recomputed, the same document renamed_document()
  // and with_integrity() give. When unset, a rename goes through save_draft
  // and admit.
  std::function<bool(const std::string& id, std::string_view name,
                     std::string* error)>
      rename;
};

class AreaCatalog {
public:
  // A territory and its home location, whose style new areas take. Adding
  // a territory again replaces its home location (a newer revision) and
  // keeps its areas.
  void add_territory(std::shared_ptr<const LocationDefinition> home);
  [[nodiscard]] bool has_territory(std::string_view territory) const;
  [[nodiscard]] std::shared_ptr<const LocationDefinition>
  home(std::string_view territory) const;

  struct AddResult {
    const Area* area{nullptr}; // null: nothing was added
    Findings findings;
    // For a draft: what it needs before Play.
    std::vector<std::string> missing;
  };
  // Add Area: Automatic is generated, validated and admitted; Empty is a
  // draft. A failure (unknown territory, bad name, generation or store
  // refused) leaves the catalog as it was. Without a seed the area's id
  // picks one, so it differs per area and stays reproducible.
  AddResult add_area(std::string_view territory, std::string_view name,
                     std::optional<std::uint64_t> seed, AreaMode mode,
                     std::string created_utc = "2026-11-14T00:00:00Z");

  // The territory's pages: the home location first, then its areas in
  // order (drafts included, marked).
  [[nodiscard]] std::vector<AreaPage> pages(std::string_view territory) const;

  [[nodiscard]] const Area* find(std::string_view id) const;
  [[nodiscard]] const Area* find_by_key(const ContentKey& key) const;

  // Renames an area: display name only, revision + 1, same id and key.
  // Games that hold the area keep the revision they pinned.
  bool rename(std::string_view id, std::string_view name, std::string* error);

  // Why the area cannot be played: its missing requirements, or "it is a
  // draft". Empty when it is admitted.
  [[nodiscard]] std::vector<std::string>
  play_problems(std::string_view id) const;

  // An admitted area the catalog has not seen, from a game that holds it
  // (a save loaded in a process whose catalog lacks it): listed under its
  // territory with an id of this catalog. The known area when its key is
  // already listed; null for a definition of no known territory or the
  // territory's home location.
  const Area* adopt(std::shared_ptr<const LocationDefinition> definition);

  void hooks(AreaStoreHooks hooks) { hooks_ = std::move(hooks); }

  // Everything forgotten (tests; a new profile).
  void clear();

private:
  struct Territory {
    std::string id;
    std::shared_ptr<const LocationDefinition> home;
  };
  std::vector<Territory> territories_;
  std::vector<Area> areas_; // in creation order
  int next_id_{1};
  AreaStoreHooks hooks_;

  Area* find_mut(std::string_view id);
  [[nodiscard]] std::string key_namespace() const;
};

// The catalog of this process.
[[nodiscard]] AreaCatalog& area_catalog();

} // namespace core::content
