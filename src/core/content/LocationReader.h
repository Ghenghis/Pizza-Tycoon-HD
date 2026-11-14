/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "core/content/Findings.h"
#include "core/content/LocationDefinition.h"

namespace core::content {

// The canonical text of a document for hashing: sorted keys, no whitespace,
// UTF-8. For documents of integers, strings, booleans and nulls this is the
// RFC 8785 (JCS) form the contract pins (location-contract.md section 3).
[[nodiscard]] std::string canonical_json(const nlohmann::json& document);

// SHA-256 of the canonical text of document without its integrity member.
[[nodiscard]] std::string definition_sha256(const nlohmann::json& document);

struct LocationReadResult {
  // Set when the document has the shape of location.schema.json (no
  // SCHEMA-1 finding); the semantic rules have not run yet.
  std::shared_ptr<const LocationDefinition> definition;
  Findings findings;
};

// Stage 1 of the validation pipeline (location-contract.md section 5): the
// shape location.schema.json describes, reported as SCHEMA-1 with the JSON
// pointer of the first offending value. It checks every member the runtime
// reads (presence, type, range, pattern, enumeration, array sizes, unknown
// members) and the identity rules of the schema (format, schema_version,
// location_key outside classic: and qa-synthetic:). The full JSON Schema
// stays the reference for tools.
[[nodiscard]] LocationReadResult read_location(const nlohmann::json& document);
[[nodiscard]] LocationReadResult read_location_text(std::string_view text);
[[nodiscard]] LocationReadResult
read_location_file(const std::filesystem::path& path);

} // namespace core::content
