/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Add Area (WORLD-BUILDER-EXPANSION.md): a new local area of a territory in
// the style of the territory's home location, as a complete location
// document. Template-based and deterministic: the same request gives the same
// document, byte for byte; no simulation random stream and no external
// service is involved.
//
// Automatic builds a playable proposal: a ring road with an avenue and a
// street crossing at a square, about forty reachable premises in the home
// location's size mix and a depot, the eight named buildings, six districts
// of the inherited kit (market, workshops, square, meadows, gardens,
// harbour) with the home location's demand, prices and suppliers, and the
// territory's art by reference (<territory>:gfx/landsym). It passes the
// schema and the validator with no findings.
//
// Start empty builds a draft in the same style with no roads and no
// premises: it parses, but it may not be played until its requirements are
// met (missing_requirements).

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"

namespace core::content {

enum class AreaMode : uint8_t { Automatic, Empty };

[[nodiscard]] std::string_view area_mode_name(AreaMode mode);

struct AreaRequest {
  // The territory: the home location's region id or bonus route
  // ("north-pole").
  std::string territory;
  // The home location whose style the area inherits (Santa's Pizza Village).
  std::shared_ptr<const LocationDefinition> style;
  std::string name;
  std::uint64_t seed = 0;
  AreaMode mode = AreaMode::Automatic;
  // The key namespace of this installation (location-contract.md 2.1,
  // "u-<8 hex>"); the store provides it.
  std::string key_namespace = "u-00000000";
  // Location keys already in use (the catalog's); the new key avoids them.
  std::set<std::string> taken_keys;
  // provenance.created_utc; an input, so the document stays deterministic.
  std::string created_utc = "2026-11-14T00:00:00Z";
};

struct AreaProposal {
  // Empty when generation failed; findings then say why.
  nlohmann::json document;
  std::string location_key;
  std::string document_id; // provenance.project_id
  Findings findings;
  [[nodiscard]] bool generated() const { return !document.is_null(); }
};

// The area's document. Automatic: complete, with integrity, and checked with
// read_location and validate_location; any error fails the generation. Empty:
// a draft without integrity. Fails, with a finding, on a name the game
// cannot show (1 to 24 printable ISO-8859-1 characters), a style that is not
// in the territory, or no free key.
[[nodiscard]] AreaProposal generate_area(const AreaRequest& request);

// What a draft still needs before it can be played, in words a player can
// act on; empty when the document passes the schema and the validator.
[[nodiscard]] std::vector<std::string>
missing_requirements(const nlohmann::json& document);

// The document under a new display name: display.name changed, the revision
// one higher, the integrity member dropped (location-contract.md: a rename
// changes display.name only and an edit is revision + 1). Keys and ids stay.
[[nodiscard]] nlohmann::json renamed_document(const nlohmann::json& document,
                                              std::string_view name);

// The same document with integrity.definition_sha256 set to its hash.
[[nodiscard]] nlohmann::json with_integrity(nlohmann::json document);

// Whether the game can show name: 1 to 24 printable ISO-8859-1 characters
// (given as UTF-8).
[[nodiscard]] bool valid_area_name(std::string_view name);

// The lower-case slug of a name for keys ("Frostfall Hollow" ->
// "frostfall-hollow"); "area" when nothing is left.
[[nodiscard]] std::string area_slug(std::string_view name);

} // namespace core::content
