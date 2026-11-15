/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/save/UserAreaStore.h"

#include <algorithm>
#include <functional>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "core/content/LocationReader.h"
#include "core/content/LocationValidator.h"
#include "core/content/Sha256.h"
#include "engine/FormatCompat.h"
#include "engine/Logger.h"
#include "engine/PlatformPaths.h"

namespace save {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::string_view index_format = "plegacy-user-areas";
constexpr std::string_view installation_file = "installation.json";
constexpr std::string_view document_extension = ".json";
constexpr std::uint64_t max_document_size = 16ULL * 1024 * 1024;

std::string display(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

// Whether a parsed version is one to use; why not, if it isn't.
using Check = std::function<std::optional<std::string>(const json&)>;

std::optional<json> parse(std::string_view text) {
  try {
    return json::parse(text.begin(), text.end());
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

void remove_retired(const SaveSlot& slot, SlotManifest& manifest) {
  std::erase_if(manifest.retired, [&slot](std::uint64_t sequence) {
    std::error_code ec;
    fs::remove(slot.version_path(sequence), ec);
    return !ec;
  });
}

// One new version of a document, as a save version is written: listed as a
// candidate first, written under its own name, read back, checked, and only
// then made the working, active one; the older versions past three go.
// Returns why it failed; a failure changes no working version.
std::optional<std::string> write_versioned(const SaveSlot& slot,
                                           const std::string& text,
                                           std::string_view made_for,
                                           const Check& check,
                                           std::uint64_t* written_as = nullptr) {
  std::error_code ec;
  fs::create_directories(slot.folder(), ec);
  if (ec) {
    return "the folder can't be made: " + ec.message();
  }
  const SaveSlot::ManifestRead read = slot.read_manifest();
  if (read.newer) {
    return "a newer build keeps these versions";
  }
  SlotManifest manifest = read.manifest.value_or(SlotManifest{});
  remove_retired(slot, manifest);
  std::uint64_t sequence = manifest.next_sequence;
  for (const std::uint64_t on_disk : slot.version_files()) {
    sequence = std::max(sequence, on_disk + 1);
  }
  manifest.next_sequence = sequence + 1;
  const FileDigest digest = digest_of(text);
  manifest.versions.insert(
      manifest.versions.begin(),
      SaveVersion{.sequence = sequence,
                  .status = VersionStatus::Candidate,
                  .level = VersionLevel::Written,
                  .reason = "being written",
                  .made_for = std::string(made_for),
                  .written_at = utc_now(),
                  .size = digest.size,
                  .checksum = digest.checksum,
                  .fingerprint = core::content::Sha256::hex(text).substr(0, 16),
                  .game_date = ""});
  if (const util::AtomicWriteResult listed = slot.write_manifest(manifest);
      !listed) {
    return listed.error;
  }
  SaveVersion& version = *manifest.find(sequence);
  if (const util::AtomicWriteResult written =
          util::write_file_atomically(slot.version_path(sequence), text);
      !written) {
    version.reason = "it could not be written: " + written.error;
    static_cast<void>(slot.write_manifest(manifest));
    return written.error;
  }

  // Read back: the bytes written, a JSON document, and one the check takes.
  std::string reason;
  const FileRead back = read_whole_file(slot.version_path(sequence),
                                        max_document_size);
  if (!back.bytes || digest_of(*back.bytes) != digest) {
    reason = "its bytes on disk are not the ones written";
  }
  else {
    version.level = VersionLevel::IntegrityChecked;
    const std::optional<json> doc = parse(*back.bytes);
    if (!doc) {
      reason = "it is not JSON";
    }
    else if (std::optional<std::string> why = check(*doc)) {
      reason = std::move(*why);
    }
    else {
      version.level = VersionLevel::LoadVerified;
      version.status = VersionStatus::Working;
      manifest.active = sequence;
    }
  }
  version.reason = reason;
  apply_retention(manifest);
  if (const util::AtomicWriteResult committed = slot.write_manifest(manifest);
      !committed) {
    return committed.error;
  }
  if (!reason.empty()) {
    return reason;
  }
  if (const util::AtomicWriteResult refreshed =
          util::write_file_atomically(slot.file(), text);
      !refreshed) {
    Logger::warn("{} still holds an earlier version ({})",
                 display(slot.file()), refreshed.error);
  }
  remove_retired(slot, manifest);
  if (written_as != nullptr) {
    *written_as = sequence;
  }
  return std::nullopt;
}

// One version of a slot, if its bytes are still the ones listed and the
// check takes it.
std::optional<json> read_version(const SaveSlot& slot, std::uint64_t sequence,
                                 const Check& check) {
  const SaveSlot::ManifestRead read = slot.read_manifest();
  const SaveVersion* v = read.manifest ? read.manifest->find(sequence) : nullptr;
  if (v == nullptr || v->status != VersionStatus::Working) {
    return std::nullopt;
  }
  const FileRead file =
      read_whole_file(slot.version_path(sequence), max_document_size);
  if (!file.bytes || digest_of(*file.bytes) !=
                         FileDigest{.size = v->size, .checksum = v->checksum}) {
    return std::nullopt;
  }
  std::optional<json> doc = parse(*file.bytes);
  if (!doc || check(*doc)) {
    return std::nullopt;
  }
  return doc;
}

// The newest verified version that passes the check: the active one, then
// the other working ones, newest first. A damaged one falls back to the
// next. Without a readable manifest, the slot's own file.
std::optional<json> read_versioned(const SaveSlot& slot, const Check& check) {
  const auto usable = [&check](const FileRead& file,
                               const std::optional<FileDigest>& expected)
      -> std::optional<json> {
    if (!file.bytes ||
        (expected && digest_of(*file.bytes) != *expected)) {
      return std::nullopt;
    }
    std::optional<json> doc = parse(*file.bytes);
    if (!doc || check(*doc)) {
      return std::nullopt;
    }
    return doc;
  };

  const SaveSlot::ManifestRead read = slot.read_manifest();
  if (!read.manifest) {
    return usable(read_whole_file(slot.file(), max_document_size),
                  std::nullopt);
  }
  std::vector<const SaveVersion*> working;
  for (const SaveVersion& v : read.manifest->versions) {
    if (v.status == VersionStatus::Working) {
      working.push_back(&v);
    }
  }
  std::ranges::stable_partition(working, [&](const SaveVersion* v) {
    return read.manifest->active == v->sequence;
  });
  for (const SaveVersion* v : working) {
    if (std::optional<json> doc = usable(
            read_whole_file(slot.version_path(v->sequence), max_document_size),
            FileDigest{.size = v->size, .checksum = v->checksum})) {
      return doc;
    }
    Logger::warn("Version {} of {} is damaged; trying the one before",
                 v->sequence, display(slot.file()));
  }
  return std::nullopt;
}

std::optional<std::string> draft_check(const json& doc) {
  if (!doc.is_object() || doc.value("format", "") != "pthd.location") {
    return "it is not a location document";
  }
  const auto key = core::content::ContentKey::parse(doc.value("location_key", ""));
  if (!key) {
    return "it has no location key";
  }
  return std::nullopt;
}

// A playable document: it reads, passes the validator and its integrity
// hash is its own.
std::optional<std::string> admitted_check(const json& doc) {
  const core::content::LocationReadResult read = core::content::read_location(doc);
  if (!read.definition) {
    return "it does not read: " + core::content::describe(read.findings);
  }
  const core::content::Findings findings =
      core::content::validate_location(*read.definition);
  if (core::content::has_errors(findings)) {
    return "it breaks a rule: " + core::content::describe(findings);
  }
  if (read.definition->declared_sha256 != read.definition->sha256) {
    return "its integrity hash is not its own";
  }
  return std::nullopt;
}

std::optional<std::string> index_check(const json& doc) {
  if (!doc.is_object() || doc.value("format", "") != index_format ||
      !doc.contains("areas") || !doc.at("areas").is_array()) {
    return "it is not an area index";
  }
  return std::nullopt;
}

std::string_view to_string(AreaState state) {
  return state == AreaState::Admitted ? "admitted" : "draft";
}

json entry_to_json(const UserAreaEntry& e) {
  return {{"id", e.id},
          {"location_key", e.location_key},
          {"name", e.name},
          {"territory", e.territory},
          {"order", e.order},
          {"state", to_string(e.state)},
          {"revision", e.revision},
          {"sha256", e.sha256},
          {"admitted_version", e.admitted_version},
          {"created_utc", e.created_utc},
          {"updated_utc", e.updated_utc}};
}

UserAreaEntry entry_from_json(const json& j) {
  UserAreaEntry e;
  e.id = j.at("id").get<std::string>();
  e.location_key = j.at("location_key").get<std::string>();
  e.name = j.at("name").get<std::string>();
  e.territory = j.at("territory").get<std::string>();
  e.order = j.at("order").get<int>();
  const std::string state = j.at("state").get<std::string>();
  if (state != "draft" && state != "admitted") {
    throw std::runtime_error("an area has the state " + state);
  }
  e.state = state == "admitted" ? AreaState::Admitted : AreaState::Draft;
  e.revision = j.at("revision").get<int>();
  e.sha256 = j.at("sha256").get<std::string>();
  e.admitted_version = j.value("admitted_version", std::uint64_t{0});
  e.created_utc = j.at("created_utc").get<std::string>();
  e.updated_utc = j.at("updated_utc").get<std::string>();
  return e;
}

// world.placement: a region id, or the bonus route.
std::string territory_of(const json& doc) {
  const json placement =
      doc.value("world", json::object()).value("placement", json::object());
  if (placement.value("kind", "") == "bonus") {
    return placement.value("route", "");
  }
  return placement.value("region", "");
}

std::string name_of(const json& doc) {
  return doc.value("display", json::object()).value("name", "Unnamed area");
}

// Keeps a territory's areas numbered 0, 1, 2, ... in their order.
void renumber(std::vector<UserAreaEntry>& areas, const std::string& territory) {
  std::vector<UserAreaEntry*> in;
  for (UserAreaEntry& e : areas) {
    if (e.territory == territory) {
      in.push_back(&e);
    }
  }
  std::ranges::stable_sort(in, {}, &UserAreaEntry::order);
  for (std::size_t i = 0; i < in.size(); ++i) {
    in[i]->order = static_cast<int>(i);
  }
}

StoreResult failed(std::string error) {
  StoreResult r;
  r.error = std::move(error);
  return r;
}

} // namespace

UserAreaStore::UserAreaStore(fs::path root)
    : root_(root.empty() ? default_root() : std::move(root)) {}

fs::path UserAreaStore::default_root() {
  return fs::path(platform_paths::base_dir()) / "content" / "areas";
}

std::string UserAreaStore::installation_namespace() const {
  const fs::path file = root_ / installation_file;
  const FileRead read = read_whole_file(file, 4096);
  if (read.bytes) {
    if (const std::optional<json> doc = parse(*read.bytes)) {
      const std::string ns = doc->value("namespace", "");
      if (ns.size() == 10 && ns.starts_with("u-")) {
        return ns;
      }
    }
  }
  // Made once, from a source that is not the game's random stream.
  std::random_device device;
  const std::uint32_t bits = device();
  const std::string ns = compat::format("u-{:08x}", bits);
  std::error_code ec;
  fs::create_directories(root_, ec);
  if (const util::AtomicWriteResult written = util::write_file_atomically(
          file, json{{"namespace", ns}}.dump(2) + "\n");
      !written) {
    Logger::warn("Could not keep this installation's namespace: {}",
                 written.error);
  }
  return ns;
}

SaveSlot UserAreaStore::index_slot() const {
  return SaveSlot(root_ / "index.json", std::string(document_extension));
}

SaveSlot UserAreaStore::area_slot(std::string_view id, AreaState which) const {
  const std::string name = std::string(id) +
                           (which == AreaState::Draft ? ".draft" : "") +
                           std::string(document_extension);
  return SaveSlot(root_ / "areas" / name, std::string(document_extension));
}

UserAreaStore::Index UserAreaStore::read_index() const {
  Index index;
  const std::optional<json> doc = read_versioned(index_slot(), index_check);
  if (!doc) {
    return index;
  }
  try {
    index.next_id = doc->value("next_id", std::uint64_t{1});
    for (const json& e : doc->at("areas")) {
      index.areas.push_back(entry_from_json(e));
    }
  }
  catch (const std::exception& e) {
    Logger::warn("The area index can't be read: {}", e.what());
    index = Index{};
  }
  return index;
}

StoreResult UserAreaStore::write_index(const Index& index,
                                       std::string_view made_for) {
  json areas = json::array();
  for (const UserAreaEntry& e : index.areas) {
    areas.push_back(entry_to_json(e));
  }
  const json doc = {{"format", index_format},
                    {"format_version", 1},
                    {"next_id", index.next_id},
                    {"areas", std::move(areas)}};
  if (std::optional<std::string> why = write_versioned(
          index_slot(), doc.dump(2) + "\n", made_for, index_check)) {
    return failed("The area list could not be saved: " + *why);
  }
  StoreResult r;
  r.ok = true;
  return r;
}

std::vector<UserAreaEntry> UserAreaStore::list() const {
  std::vector<UserAreaEntry> areas = read_index().areas;
  std::ranges::stable_sort(areas, [](const UserAreaEntry& a,
                                     const UserAreaEntry& b) {
    return a.territory != b.territory ? a.territory < b.territory
                                      : a.order < b.order;
  });
  return areas;
}

std::optional<UserAreaEntry> UserAreaStore::find(std::string_view id) const {
  for (UserAreaEntry& e : read_index().areas) {
    if (e.id == id) {
      return e;
    }
  }
  return std::nullopt;
}

std::optional<UserAreaEntry>
UserAreaStore::find_by_key(std::string_view location_key) const {
  for (UserAreaEntry& e : read_index().areas) {
    if (e.location_key == location_key) {
      return e;
    }
  }
  return std::nullopt;
}

StoreResult UserAreaStore::save_draft(const json& document,
                                      std::optional<std::string> id) {
  if (std::optional<std::string> why = draft_check(document)) {
    return failed("This is not an area that can be kept: " + *why + ".");
  }
  Index index = read_index();
  const std::string key = document.at("location_key").get<std::string>();
  for (const UserAreaEntry& e : index.areas) {
    if (e.location_key == key && (!id || e.id != *id)) {
      return failed("Another area already has the key " + key + ".");
    }
  }

  UserAreaEntry* entry = nullptr;
  if (id) {
    const auto it = std::ranges::find(index.areas, *id, &UserAreaEntry::id);
    if (it == index.areas.end()) {
      return failed("There is no area " + *id + ".");
    }
    entry = &*it;
  }
  else {
    UserAreaEntry e;
    e.id = compat::format("area-{:06d}", index.next_id++);
    e.territory = territory_of(document);
    e.name = name_of(document);
    e.order = static_cast<int>(std::ranges::count(
        index.areas, e.territory, &UserAreaEntry::territory));
    e.created_utc = utc_now();
    index.areas.push_back(std::move(e));
    entry = &index.areas.back();
  }
  entry->location_key = key;
  entry->updated_utc = utc_now();
  const std::string area_id = entry->id;

  if (std::optional<std::string> why = write_draft(area_id, document)) {
    return failed("The draft could not be saved: " + *why);
  }
  StoreResult r = write_index(index, "draft of " + area_id);
  r.id = area_id;
  return r;
}

std::optional<std::string> UserAreaStore::write_draft(std::string_view id,
                                                      const json& document) {
  return write_versioned(area_slot(id, AreaState::Draft),
                         document.dump(2) + "\n", "draft", draft_check);
}

StoreResult UserAreaStore::admit_into(Index& index, std::string_view id) {
  const auto it = std::ranges::find(index.areas, id, &UserAreaEntry::id);
  if (it == index.areas.end()) {
    return failed("There is no area " + std::string(id) + ".");
  }
  const std::optional<json> doc = document(id, AreaState::Draft);
  if (!doc) {
    return failed("The area's draft can't be read.");
  }

  // A new admission of changed content is a new revision; a published
  // revision is never rewritten.
  json candidate = *doc;
  candidate.erase("integrity");
  int revision = candidate.value("revision", 1);
  if (!it->sha256.empty()) {
    json as_before = candidate;
    as_before["revision"] = it->revision;
    as_before["integrity"] = {
        {"canonicalization", "jcs-rfc8785"},
        {"definition_sha256", core::content::definition_sha256(as_before)}};
    const auto same = core::content::read_location(as_before);
    revision = same.definition && same.definition->sha256 == it->sha256
                   ? it->revision
                   : std::max(revision, it->revision + 1);
  }
  candidate["revision"] = revision;
  candidate["integrity"] = {{"canonicalization", "jcs-rfc8785"},
                            {"definition_sha256",
                             core::content::definition_sha256(candidate)}};

  const core::content::LocationReadResult read =
      core::content::read_location(candidate);
  StoreResult result;
  if (!read.definition) {
    result.findings = read.findings;
    result.error = "The area can't be played yet: it does not read.";
    return result;
  }
  result.findings = core::content::validate_location(*read.definition);
  if (core::content::has_errors(result.findings)) {
    result.error = "The area can't be played yet: " +
                   core::content::describe(result.findings);
    return result;
  }

  std::uint64_t version = 0;
  if (std::optional<std::string> why = write_versioned(
          area_slot(id, AreaState::Admitted), candidate.dump(2) + "\n",
          "admitted revision " + std::to_string(revision), admitted_check,
          &version)) {
    return failed("The area could not be admitted: " + *why);
  }
  it->state = AreaState::Admitted;
  it->revision = revision;
  it->sha256 = read.definition->sha256;
  it->admitted_version = version;
  it->updated_utc = utc_now();
  result.ok = true;
  result.id = std::string(id);
  return result;
}

StoreResult UserAreaStore::admit(std::string_view id) {
  Index index = read_index();
  StoreResult result = admit_into(index, id);
  if (!result.ok) {
    return result;
  }
  StoreResult r = write_index(index, "admitted " + std::string(id));
  r.id = std::string(id);
  r.findings = std::move(result.findings);
  return r;
}

StoreResult UserAreaStore::rename(std::string_view id, std::string_view name) {
  Index index = read_index();
  const auto it = std::ranges::find(index.areas, id, &UserAreaEntry::id);
  if (it == index.areas.end()) {
    return failed("There is no area " + std::string(id) + ".");
  }
  if (name.empty()) {
    return failed("An area needs a name.");
  }
  // A rename changes display.name only (location contract). An admitted
  // area renames its admitted document: revision + 1 once, integrity
  // recomputed by admitting it again, the same document the catalog's
  // renamed_document() and with_integrity() give. A draft renames the draft.
  // Key, id and order never change. One index write, so one undo takes it
  // all back.
  const bool admitted = it->state == AreaState::Admitted;
  if (std::optional<json> doc =
          document(id, admitted ? AreaState::Admitted : AreaState::Draft)) {
    (*doc)["display"]["name"] = std::string(name);
    doc->erase("integrity");
    if (admitted) {
      (*doc)["revision"] = it->revision + 1;
    }
    if (std::optional<std::string> why = write_draft(id, *doc)) {
      return failed("The draft could not be saved: " + *why);
    }
    if (admitted) {
      if (StoreResult again = admit_into(index, id); !again.ok) {
        return again;
      }
    }
  }
  it->name = std::string(name);
  it->updated_utc = utc_now();
  StoreResult r = write_index(index, "renamed " + std::string(id));
  r.id = std::string(id);
  return r;
}

StoreResult UserAreaStore::reorder(std::string_view id, int new_order) {
  Index index = read_index();
  const auto it = std::ranges::find(index.areas, id, &UserAreaEntry::id);
  if (it == index.areas.end()) {
    return failed("There is no area " + std::string(id) + ".");
  }
  const std::string territory = it->territory;
  renumber(index.areas, territory);
  const int from = it->order;
  const int count = static_cast<int>(
      std::ranges::count(index.areas, territory, &UserAreaEntry::territory));
  const int to = std::clamp(new_order, 0, count - 1);
  for (UserAreaEntry& e : index.areas) {
    if (e.territory != territory || e.id == it->id) {
      continue;
    }
    if (from < to && e.order > from && e.order <= to) {
      --e.order;
    }
    else if (to < from && e.order >= to && e.order < from) {
      ++e.order;
    }
  }
  it->order = to;
  StoreResult r = write_index(index, "reordered " + std::string(id));
  r.id = std::string(id);
  return r;
}

StoreResult UserAreaStore::withdraw(std::string_view id) {
  Index index = read_index();
  const auto it = std::ranges::find(index.areas, id, &UserAreaEntry::id);
  if (it == index.areas.end()) {
    return failed("There is no area " + std::string(id) + ".");
  }
  it->state = AreaState::Draft;
  it->updated_utc = utc_now();
  StoreResult r = write_index(index, "withdrew " + std::string(id));
  r.id = std::string(id);
  return r;
}

StoreResult UserAreaStore::undo_last_change() {
  const SaveSlot slot = index_slot();
  SaveSlot::ManifestRead read = slot.read_manifest();
  if (!read.manifest || !read.manifest->active) {
    return failed("There is nothing to undo.");
  }
  SlotManifest& manifest = *read.manifest;
  const SaveVersion* before = nullptr;
  for (const SaveVersion& v : manifest.versions) {
    if (v.status == VersionStatus::Working && v.sequence < *manifest.active &&
        (before == nullptr || v.sequence > before->sequence)) {
      before = &v;
    }
  }
  if (before == nullptr) {
    return failed("There is nothing to undo.");
  }
  const FileRead bytes =
      read_whole_file(slot.version_path(before->sequence), max_document_size);
  if (!bytes.bytes ||
      digest_of(*bytes.bytes) !=
          FileDigest{.size = before->size, .checksum = before->checksum}) {
    return failed("The earlier area list is damaged, so nothing was undone.");
  }
  manifest.previous_active = manifest.active;
  manifest.active = before->sequence;
  if (const util::AtomicWriteResult written = slot.write_manifest(manifest);
      !written) {
    return failed("The area list could not be saved: " + written.error);
  }
  static_cast<void>(util::write_file_atomically(slot.file(), *bytes.bytes));
  StoreResult r;
  r.ok = true;
  return r;
}

std::optional<json> UserAreaStore::document(std::string_view id,
                                            AreaState which) const {
  if (which == AreaState::Admitted) {
    // The version the index says the area plays; the newest working one if
    // that is gone or damaged.
    if (const std::optional<UserAreaEntry> entry = find(id);
        entry && entry->admitted_version != 0) {
      if (std::optional<json> doc =
              read_version(area_slot(id, which), entry->admitted_version,
                           admitted_check)) {
        return doc;
      }
      Logger::warn("Area {}'s admitted version {} can't be read; using the "
                   "newest verified one",
                   id, entry->admitted_version);
    }
  }
  return read_versioned(area_slot(id, which), which == AreaState::Admitted
                                                  ? Check(admitted_check)
                                                  : Check(draft_check));
}

std::vector<std::shared_ptr<const core::content::LocationDefinition>>
UserAreaStore::admitted(std::string_view territory) const {
  std::vector<std::shared_ptr<const core::content::LocationDefinition>> out;
  for (const UserAreaEntry& e : list()) {
    if (e.territory != territory || e.state != AreaState::Admitted) {
      continue;
    }
    if (const std::optional<json> doc = document(e.id, AreaState::Admitted)) {
      core::content::LocationReadResult read = core::content::read_location(*doc);
      if (read.definition) {
        out.push_back(std::move(read.definition));
      }
    }
  }
  return out;
}

StoreResult
UserAreaStore::keep_copy(const core::content::LocationDefinition& definition) {
  const std::optional<json> doc = parse(definition.canonical_document);
  if (!doc) {
    return failed("The save's copy of the area can't be read.");
  }
  if (const std::optional<UserAreaEntry> existing =
          find_by_key(definition.key.str())) {
    // The player's own edits stay; the copy becomes the draft to compare.
    return save_draft(*doc, existing->id);
  }
  StoreResult saved = save_draft(*doc);
  if (!saved.ok) {
    return saved;
  }
  return admit(saved.id);
}

std::vector<AreaDifference> area_differences(
    const std::vector<std::shared_ptr<const core::content::LocationDefinition>>&
        embedded,
    const UserAreaStore& store) {
  std::vector<AreaDifference> out;
  for (const auto& definition : embedded) {
    if (!definition || !definition->key.namespace_part().starts_with("u-")) {
      continue;
    }
    const std::optional<UserAreaEntry> entry =
        store.find_by_key(definition->key.str());
    const bool missing = !entry || entry->state != AreaState::Admitted ||
                         !store.document(entry->id, AreaState::Admitted);
    if (missing || entry->sha256 != definition->sha256) {
      out.push_back({.location_key = definition->key.str(),
                     .name = definition->name,
                     .missing = missing});
    }
  }
  return out;
}

} // namespace save
