/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/JsonStore.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include "core/content/Sha256.h"
#include "engine/music/MusicTypes.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace music {

namespace {

// Thrown by the parser callback to stop at a limit.
class TooBig : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

constexpr int max_depth = 16;
constexpr std::uint64_t max_values = 4'000'000;

} // namespace

std::optional<json> parse_json(std::string_view text, int depth_limit,
                               std::uint64_t value_limit, std::string* error) {
  std::uint64_t values = 0;
  const json::parser_callback_t check = [&](int depth,
                                            json::parse_event_t event, json&) {
    if (depth > depth_limit) {
      throw TooBig("nested more than " + std::to_string(depth_limit) + " deep");
    }
    if ((event == json::parse_event_t::object_start ||
         event == json::parse_event_t::array_start ||
         event == json::parse_event_t::value) &&
        ++values > value_limit) {
      throw TooBig("more than " + std::to_string(value_limit) + " values");
    }
    return true;
  };
  try {
    return json::parse(text.data(), text.data() + text.size(), check);
  }
  catch (const TooBig& e) {
    if (error) {
      *error = e.what();
    }
  }
  catch (const json::exception& e) {
    if (error) {
      *error = e.what();
    }
  }
  return std::nullopt;
}

std::optional<std::string> read_text_file(const fs::path& file,
                                          std::uint64_t max_bytes,
                                          std::string* error) {
  std::error_code ec;
  const auto status = fs::status(file, ec);
  if (ec || !fs::exists(status)) {
    return std::nullopt;
  }
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec) {
    if (error) {
      *error = "cannot be read: " + ec.message();
    }
    return std::nullopt;
  }
  if (size > max_bytes) {
    if (error) {
      *error = "is larger than " + mib(max_bytes);
    }
    return std::nullopt;
  }
  std::ifstream in(file, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (!in && !in.eof()) {
    if (error) {
      *error = "cannot be read";
    }
    return std::nullopt;
  }
  return text;
}

RevisionedDocument::RevisionedDocument(fs::path file, std::string format,
                                       int schema)
    : file_(std::move(file)), format_(std::move(format)), schema_(schema) {}

fs::path RevisionedDocument::candidate_path() const {
  fs::path p = file_;
  p += ".candidate";
  return p;
}

fs::path RevisionedDocument::backup_path(int n) const {
  fs::path p = file_;
  p += "." + std::to_string(n);
  return p;
}

fs::path RevisionedDocument::damaged_path() const {
  fs::path p = file_;
  p += ".damaged";
  return p;
}

std::optional<json> RevisionedDocument::read_copy(const fs::path& path,
                                                  std::string* why) const {
  std::string error;
  const std::optional<std::string> text =
      read_text_file(path, MAX_BYTES, &error);
  if (!text) {
    *why = error.empty() ? "is not there" : error;
    return std::nullopt;
  }
  std::optional<json> doc = parse_json(*text, max_depth, max_values, &error);
  if (!doc) {
    *why = "is damaged (" + error + ")";
    return std::nullopt;
  }
  if (!doc->is_object() || doc->value("format", "") != format_) {
    *why = "is not a " + format_ + " file";
    return std::nullopt;
  }
  const auto schema = doc->find("schema");
  if (schema == doc->end() || !schema->is_number_integer() ||
      *schema != schema_) {
    *why = "has a schema this game does not read";
    return std::nullopt;
  }
  const auto data = doc->find("data");
  const auto sum = doc->find("sha256");
  if (data == doc->end() || sum == doc->end() || !sum->is_string()) {
    *why = "is damaged (no data or checksum)";
    return std::nullopt;
  }
  if (core::content::Sha256::hex(data->dump()) != sum->get<std::string>()) {
    *why = "is damaged (its checksum does not match)";
    return std::nullopt;
  }
  return std::move(*data);
}

std::optional<RevisionedDocument::Loaded>
RevisionedDocument::load(std::vector<std::string>* problems) const {
  std::vector<std::pair<fs::path, std::string>> copies = {
      {file_, "current"}, {candidate_path(), "candidate"}};
  for (int n = 1; n <= BACKUPS; ++n) {
    copies.emplace_back(backup_path(n), "backup " + std::to_string(n));
  }
  for (const auto& [path, name] : copies) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      continue;
    }
    std::string why;
    if (std::optional<json> data = read_copy(path, &why)) {
      return Loaded{.data = std::move(*data), .source = name};
    }
    if (problems) {
      problems->push_back("The " + name + " copy of " +
                          utf8_path(file_.filename()) + " " + why);
    }
  }
  return std::nullopt;
}

util::AtomicWriteResult RevisionedDocument::save(const json& data) {
  json doc = {
      {"format", format_},
      {"schema", schema_},
      {"sha256", core::content::Sha256::hex(data.dump())},
      {"data", data},
  };
  const fs::path candidate = candidate_path();
  util::AtomicWriteResult written =
      util::write_file_atomically(candidate, doc.dump(2) + "\n");
  if (!written) {
    return written;
  }
  std::string why;
  if (!read_copy(candidate, &why)) {
    return {.error = "the new " + utf8_path(file_.filename()) + " " + why,
            .code = std::make_error_code(std::errc::io_error)};
  }

  std::error_code ec;
  if (fs::exists(file_, ec)) {
    std::string current_why;
    if (read_copy(file_, &current_why)) {
      // The current copy worked: it becomes the newest backup.
      fs::remove(backup_path(BACKUPS), ec);
      for (int n = BACKUPS - 1; n >= 1; --n) {
        if (fs::exists(backup_path(n), ec)) {
          fs::rename(backup_path(n), backup_path(n + 1), ec);
        }
      }
      fs::rename(file_, backup_path(1), ec);
    }
    else {
      fs::rename(file_, damaged_path(), ec);
    }
    if (ec) {
      return {.error = "cannot keep the old " + utf8_path(file_.filename()) +
                       ": " + ec.message(),
              .code = ec};
    }
  }
  fs::rename(candidate, file_, ec);
  if (ec) {
    return {.error = "cannot replace " + utf8_path(file_.filename()) + ": " +
                     ec.message(),
            .code = ec};
  }
  return {};
}

} // namespace music
