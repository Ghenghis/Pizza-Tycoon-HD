/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/AreaCatalog.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"
#include "core/content/Sha256.h"

namespace core::content {

using nlohmann::json;

namespace {

Finding error(std::string rule, std::string message) {
  return Finding{.rule = std::move(rule),
                 .severity = Severity::Error,
                 .pointer = "",
                 .message = std::move(message)};
}

std::string area_id(int n) {
  char text[16];
  std::snprintf(text, sizeof text, "area-%06d", n);
  return text;
}

// A seed from the area's id and name, when none is given: different for
// every area, the same every time for the same area.
std::uint64_t default_seed(std::string_view id, std::string_view name) {
  const std::string digest =
      Sha256::hex(std::string(id) + "|" + std::string(name));
  std::uint64_t seed = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    const char c = digest[i];
    seed = (seed << 4) |
           static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return seed;
}

std::shared_ptr<const LocationDefinition>
admitted_definition(const json& document, Findings& findings) {
  auto read = read_location(document);
  findings.insert(findings.end(), read.findings.begin(), read.findings.end());
  if (!read.definition) {
    return nullptr;
  }
  const auto semantic = validate_location(*read.definition);
  findings.insert(findings.end(), semantic.begin(), semantic.end());
  return has_errors(findings) ? nullptr : read.definition;
}

} // namespace

std::string_view area_state_name(AreaState state) {
  return state == AreaState::Admitted ? "playable" : "draft";
}

std::string territory_of(const LocationDefinition& definition) {
  return definition.bonus ? definition.bonus_route : definition.region;
}

void AreaCatalog::add_territory(
    std::shared_ptr<const LocationDefinition> home) {
  if (!home) {
    return;
  }
  const std::string id = territory_of(*home);
  for (auto& t : territories_) {
    if (t.id == id) {
      t.home = std::move(home);
      return;
    }
  }
  territories_.push_back(Territory{.id = id, .home = std::move(home)});
}

bool AreaCatalog::has_territory(std::string_view territory) const {
  return home(territory) != nullptr;
}

std::shared_ptr<const LocationDefinition>
AreaCatalog::home(std::string_view territory) const {
  for (const auto& t : territories_) {
    if (t.id == territory) {
      return t.home;
    }
  }
  return nullptr;
}

std::string AreaCatalog::key_namespace() const {
  if (hooks_.installation_namespace) {
    if (std::string ns = hooks_.installation_namespace(); !ns.empty()) {
      return ns;
    }
  }
  return "u-00000000";
}

AreaCatalog::AddResult AreaCatalog::add_area(std::string_view territory,
                                             std::string_view name,
                                             std::optional<std::uint64_t> seed,
                                             AreaMode mode,
                                             std::string created_utc) {
  AddResult result;
  const auto style = home(territory);
  if (!style) {
    result.findings.push_back(
        error("AREA-2", "no territory '" + std::string(territory) +
                            "': a territory is known by its home location"));
    return result;
  }
  // The id a new in-memory area would get; the store may give another.
  const std::string provisional_id = area_id(next_id_);

  AreaRequest request;
  request.territory = std::string(territory);
  request.style = style;
  request.name = std::string(name);
  request.seed = seed.value_or(default_seed(provisional_id, name));
  request.mode = mode;
  request.key_namespace = key_namespace();
  request.created_utc = std::move(created_utc);
  for (const auto& t : territories_) {
    request.taken_keys.insert(t.home->key.str());
  }
  for (const auto& a : areas_) {
    request.taken_keys.insert(a.key.str());
  }

  AreaProposal proposal = generate_area(request);
  result.findings = proposal.findings;
  if (!proposal.generated()) {
    return result; // nothing changed
  }
  const auto key = ContentKey::parse(proposal.location_key);
  if (!key) {
    result.findings.push_back(error("AREA-3", "the area got no valid key"));
    return result;
  }

  Area area;
  area.territory = std::string(territory);
  area.name = std::string(name);
  area.key = *key;
  area.document_id = proposal.document_id;
  area.mode = mode;
  area.seed = request.seed;
  area.document = std::move(proposal.document);
  int order = 0;
  for (const auto& a : areas_) {
    if (a.territory == territory) {
      order = std::max(order, a.order + 1);
    }
  }
  area.order = order;

  if (mode == AreaMode::Automatic) {
    Findings findings;
    area.definition = admitted_definition(area.document, findings);
    if (!area.definition) {
      result.findings.insert(result.findings.end(), findings.begin(),
                             findings.end());
      return result; // nothing changed
    }
    area.state = AreaState::Admitted;
  }
  else {
    result.missing = missing_requirements(area.document);
  }

  // The store first: a refusal leaves everything as it was.
  area.id = provisional_id;
  if (hooks_.save_draft) {
    std::string why;
    const auto stored = hooks_.save_draft(area.document, std::nullopt, &why);
    if (!stored) {
      result.findings.push_back(
          error("AREA-5", "the area could not be stored: " + why));
      return result;
    }
    area.id = *stored;
    if (area.state == AreaState::Admitted && hooks_.admit) {
      Findings refused;
      if (!hooks_.admit(area.id, &refused)) {
        // Stored as a draft; the catalog shows it as one.
        area.state = AreaState::Draft;
        area.definition.reset();
        result.findings.insert(result.findings.end(), refused.begin(),
                               refused.end());
        result.missing = missing_requirements(area.document);
      }
    }
  }
  ++next_id_;
  areas_.push_back(std::move(area));
  result.area = &areas_.back();
  return result;
}

std::vector<AreaPage> AreaCatalog::pages(std::string_view territory) const {
  std::vector<AreaPage> out;
  const auto h = home(territory);
  if (!h) {
    return out;
  }
  out.push_back(AreaPage{.page = 1,
                         .area_id = {},
                         .key = h->key,
                         .name = h->name,
                         .state = AreaState::Admitted});
  std::vector<const Area*> mine;
  for (const auto& a : areas_) {
    if (a.territory == territory) {
      mine.push_back(&a);
    }
  }
  std::stable_sort(mine.begin(), mine.end(), [](const Area* a, const Area* b) {
    return a->order < b->order;
  });
  for (const Area* a : mine) {
    out.push_back(AreaPage{.page = static_cast<int>(out.size()) + 1,
                           .area_id = a->id,
                           .key = a->key,
                           .name = a->name,
                           .state = a->state});
  }
  return out;
}

const Area* AreaCatalog::find(std::string_view id) const {
  for (const auto& a : areas_) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

Area* AreaCatalog::find_mut(std::string_view id) {
  for (auto& a : areas_) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

const Area* AreaCatalog::find_by_key(const ContentKey& key) const {
  for (const auto& a : areas_) {
    if (a.key == key) {
      return &a;
    }
  }
  return nullptr;
}

bool AreaCatalog::rename(std::string_view id, std::string_view name,
                         std::string* error_out) {
  const auto fail = [&](std::string why) {
    if (error_out) {
      *error_out = std::move(why);
    }
    return false;
  };
  Area* area = find_mut(id);
  if (!area) {
    return fail("no area " + std::string(id));
  }
  if (!valid_area_name(name)) {
    return fail("'" + std::string(name) +
                "' cannot be an area name: 1 to 24 printable characters of "
                "the game's character set");
  }
  json document = renamed_document(area->document, name);
  std::shared_ptr<const LocationDefinition> definition;
  if (area->state == AreaState::Admitted) {
    document = with_integrity(std::move(document));
    Findings findings;
    definition = admitted_definition(document, findings);
    if (!definition) {
      return fail("the renamed area breaks a rule:\n" + describe(findings));
    }
  }
  if (hooks_.rename) {
    std::string why;
    if (!hooks_.rename(std::string(id), name, &why)) {
      return fail("the area could not be renamed in the store: " + why);
    }
  }
  else if (hooks_.save_draft) {
    std::string why;
    if (!hooks_.save_draft(document, std::string(id), &why)) {
      return fail("the renamed area could not be stored: " + why);
    }
    if (definition && hooks_.admit) {
      Findings refused;
      if (!hooks_.admit(std::string(id), &refused)) {
        return fail("the renamed area was stored but not admitted:\n" +
                    describe(refused));
      }
    }
  }
  area->name = std::string(name);
  area->document = std::move(document);
  if (definition) {
    area->definition = std::move(definition);
  }
  return true;
}

std::vector<std::string> AreaCatalog::play_problems(std::string_view id) const {
  const Area* area = find(id);
  if (!area) {
    return {"There is no area " + std::string(id) + "."};
  }
  if (area->state == AreaState::Admitted && area->definition) {
    return {};
  }
  auto missing = missing_requirements(area->document);
  if (missing.empty()) {
    missing.push_back("Validate the area to admit it into play.");
  }
  return missing;
}

const Area*
AreaCatalog::adopt(std::shared_ptr<const LocationDefinition> definition) {
  if (!definition) {
    return nullptr;
  }
  if (const Area* known = find_by_key(definition->key)) {
    return known;
  }
  const std::string territory = territory_of(*definition);
  const auto h = home(territory);
  if (!h || h->key == definition->key) {
    return nullptr;
  }
  Area area;
  area.id = area_id(next_id_++);
  area.territory = territory;
  area.name = definition->name;
  area.key = definition->key;
  area.state = AreaState::Admitted;
  area.document = json::parse(definition->canonical_document);
  area.document_id = area.document.at("provenance").value("project_id", "");
  area.mode = area.document.at("provenance").value("origin", "") == "manual"
                  ? AreaMode::Empty
                  : AreaMode::Automatic;
  int order = 0;
  for (const auto& a : areas_) {
    if (a.territory == territory) {
      order = std::max(order, a.order + 1);
    }
  }
  area.order = order;
  area.definition = std::move(definition);
  areas_.push_back(std::move(area));
  return &areas_.back();
}

void AreaCatalog::clear() {
  territories_.clear();
  areas_.clear();
  next_id_ = 1;
  hooks_ = {};
}

AreaCatalog& area_catalog() {
  static AreaCatalog catalog;
  return catalog;
}

} // namespace core::content
