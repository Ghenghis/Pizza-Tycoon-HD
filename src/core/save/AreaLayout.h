/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Duplicate layout: an area's design copied into a new area of its own. The
// copy gets a new location key and document id, every entity id is renamed
// (districts, premises, actors, suppliers, attachments) and every reference
// to one follows, and its integrity hash goes, so it is a draft to admit.
// A location document holds design only: who owns what, money and progress
// live in saves and are never part of it.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/content/Findings.h"

namespace save {

struct DuplicatedLayout {
  nlohmann::json document; // the copy, a draft without an integrity member
  core::content::Findings findings; // read_location and validator findings
  bool ok = false;                  // read and validated without errors
};

// new_key is the copy's location_key ("u-3f9a1c2e:north-pole-copy"),
// new_project_id its provenance.project_id, new_name its display name.
[[nodiscard]] DuplicatedLayout
duplicate_layout(const nlohmann::json& document, std::string_view new_key,
                 std::string_view new_project_id, std::string_view new_name);

} // namespace save
