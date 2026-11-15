/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/AreaLayout.h"

#include <map>
#include <set>

#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"
#include "core/content/Sha256.h"
#include "core/save/SaveVersions.h"

namespace save {

using nlohmann::json;

namespace {

// Entity ids are at most 48 characters (location.schema.json entity_id).
constexpr std::size_t max_id_length = 48;

// The entity lists of a location document: every member holding ids that
// must stay unique across the document (ID-1).
std::vector<json*> entity_lists(json& doc) {
  std::vector<json*> lists;
  for (const char* key : {"districts", "properties", "food_suppliers"}) {
    if (doc.contains(key) && doc[key].is_array()) {
      lists.push_back(&doc[key]);
    }
  }
  if (doc.contains("actors") && doc["actors"].is_object()) {
    for (auto& [role, value] : doc["actors"].items()) {
      if (value.is_array()) {
        lists.push_back(&value);
      }
    }
  }
  if (doc.contains("assets") && doc["assets"].contains("attachments") &&
      doc["assets"]["attachments"].is_array()) {
    lists.push_back(&doc["assets"]["attachments"]);
  }
  return lists;
}

void rename(json& value, const std::map<std::string, std::string>& renamed) {
  if (value.is_string()) {
    if (const auto it = renamed.find(value.get<std::string>());
        it != renamed.end()) {
      value = it->second;
    }
  }
}

} // namespace

DuplicatedLayout duplicate_layout(const json& document, std::string_view new_key,
                                  std::string_view new_project_id,
                                  std::string_view new_name) {
  DuplicatedLayout out;
  json doc = document;
  if (!doc.is_object()) {
    return out;
  }

  // Every entity id, old to new: the old one with a tag from the new key, so
  // no id is shared with the area it came from.
  const std::string tag =
      core::content::Sha256::hex(std::string(new_key)).substr(0, 6);
  std::map<std::string, std::string> renamed;
  std::set<std::string> taken;
  const auto fresh = [&](const std::string& old) {
    const std::string suffix = "-" + tag;
    std::string base = old.substr(0, max_id_length - suffix.size());
    std::string id = base + suffix;
    for (int n = 2; taken.contains(id) || id == old; ++n) {
      const std::string more = "-" + std::to_string(n);
      id = base.substr(0, max_id_length - suffix.size() - more.size()) + more +
           suffix;
    }
    taken.insert(id);
    return id;
  };
  std::vector<json*> lists = entity_lists(doc);
  json* mayor = doc.contains("actors") && doc["actors"].contains("mayor") &&
                        doc["actors"]["mayor"].is_object()
                    ? &doc["actors"]["mayor"]
                    : nullptr;
  const auto take = [&](json& entity) {
    if (entity.is_object() && entity.contains("id") &&
        entity["id"].is_string()) {
      const std::string old = entity["id"].get<std::string>();
      const std::string id = fresh(old);
      renamed[old] = id;
      entity["id"] = id;
    }
  };
  for (json* list : lists) {
    for (json& entity : *list) {
      take(entity);
    }
  }
  if (mayor != nullptr) {
    take(*mayor);
  }

  // The references: a premise's realtor, and the thumbnail's attachment.
  if (doc.contains("properties") && doc["properties"].is_array()) {
    for (json& property : doc["properties"]) {
      if (property.is_object() && property.contains("realtor")) {
        rename(property["realtor"], renamed);
      }
    }
  }
  if (doc.contains("assets") && doc["assets"].contains("thumbnail") &&
      doc["assets"]["thumbnail"].is_object() &&
      doc["assets"]["thumbnail"].contains("attachment")) {
    rename(doc["assets"]["thumbnail"]["attachment"], renamed);
  }

  // A new area: its own key, document id and name, revision 1, made from
  // another, and no integrity hash until it is admitted.
  doc["location_key"] = std::string(new_key);
  doc["revision"] = 1;
  if (doc.contains("display") && doc["display"].is_object()) {
    doc["display"]["name"] = std::string(new_name);
    doc["display"]["aliases"] = json::array();
  }
  if (doc.contains("provenance") && doc["provenance"].is_object()) {
    json& provenance = doc["provenance"];
    provenance["project_id"] = std::string(new_project_id);
    if (provenance.value("origin", "") == "generated") {
      provenance["origin"] = "mixed";
    }
    provenance["created_utc"] = utc_now();
  }
  doc.erase("integrity");

  const core::content::LocationReadResult read = core::content::read_location(doc);
  out.findings = read.findings;
  if (read.definition) {
    const core::content::Findings rules =
        core::content::validate_location(*read.definition);
    out.findings.insert(out.findings.end(), rules.begin(), rules.end());
    out.ok = !core::content::has_errors(out.findings);
  }
  out.document = std::move(doc);
  return out;
}

} // namespace save
