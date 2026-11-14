/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/content/Findings.h"

namespace core::content {

// Shape checks for the content readers: each failure is one finding (rule
// SCHEMA-1 unless told otherwise) at the JSON pointer of the value, in the
// wording of a JSON Schema validator ("missing member", "above maximum").
// Readers check the shape of a member where they read it, so what is checked
// and what is used cannot drift apart.
class JsonShape {
public:
  using json = nlohmann::json;
  // A pattern for strings, as a predicate over the whole string.
  using Pattern = bool (*)(std::string_view);

  explicit JsonShape(Findings& out, std::string rule = "SCHEMA-1")
      : out_(out), rule_(std::move(rule)) {}

  [[nodiscard]] bool failed() const { return failures_ > 0; }
  void fail(const std::string& pointer, std::string message);

  [[nodiscard]] static std::string child(const std::string& pointer,
                                         std::string_view key);
  [[nodiscard]] static std::string child(const std::string& pointer,
                                         std::size_t index);

  // value is an object, has every required member and no member outside
  // allowed (required members count as allowed).
  bool object(const json& value, const std::string& pointer,
              std::initializer_list<std::string_view> allowed,
              std::initializer_list<std::string_view> required);

  // The member, or nullptr when absent (object() reports absent required
  // members, so these never do).
  [[nodiscard]] static const json* member(const json& object,
                                          std::string_view key);

  std::optional<std::int64_t> integer(const json& value,
                                      const std::string& pointer,
                                      std::int64_t minimum,
                                      std::int64_t maximum);
  std::optional<bool> boolean(const json& value, const std::string& pointer);
  // A string of min..max code points; latin1 limits it to U+0020..U+007E and
  // U+00A0..U+00FF (the original fonts); pattern, when set, must match.
  std::optional<std::string> text(const json& value, const std::string& pointer,
                                  std::size_t min_length,
                                  std::size_t max_length, bool latin1 = false,
                                  Pattern pattern = nullptr);
  // The index of the option the string equals.
  std::optional<std::size_t>
  choice(const json& value, const std::string& pointer,
         std::initializer_list<std::string_view> options);
  // value equals the given string.
  bool constant(const json& value, const std::string& pointer,
                std::string_view expected);
  bool constant(const json& value, const std::string& pointer,
                std::int64_t expected);
  // value is an array of min..max items.
  bool array(const json& value, const std::string& pointer,
             std::size_t min_items, std::size_t max_items);
  // value is an array of distinct strings, each min..max code points and
  // matching pattern.
  bool string_set(const json& value, const std::string& pointer,
                  std::size_t max_items, std::size_t min_length,
                  std::size_t max_length, bool latin1, Pattern pattern);

  // Members read with a check, or nullopt when the member is absent or
  // malformed.
  std::optional<std::int64_t>
  integer_at(const json& object, const std::string& pointer,
             std::string_view key, std::int64_t minimum, std::int64_t maximum);
  std::optional<bool> boolean_at(const json& object, const std::string& pointer,
                                 std::string_view key);
  std::optional<std::string>
  text_at(const json& object, const std::string& pointer, std::string_view key,
          std::size_t min_length, std::size_t max_length, bool latin1 = false,
          Pattern pattern = nullptr);
  std::optional<std::size_t>
  choice_at(const json& object, const std::string& pointer,
            std::string_view key,
            std::initializer_list<std::string_view> options);

private:
  Findings& out_;
  std::string rule_;
  int failures_{0};
};

// Patterns shared by the content schemas (docs/studio/*.schema.json $defs).
namespace patterns {
// ^[a-z][a-z0-9-]{0,47}$
bool entity_id(std::string_view text);
// ^[a-z][a-z0-9-]{1,31}:[a-z0-9][a-z0-9-]{1,63}$
bool content_key(std::string_view text);
// ^[0-9a-f]{64}$
bool sha256(std::string_view text);
// ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$
bool utc(std::string_view text);
// ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$
bool semver(std::string_view text);
// ^(0|[1-9][0-9]{0,19})$
bool u64_decimal(std::string_view text);
// ^[a-z][a-z0-9.-]{2,63}$
bool dotted_id(std::string_view text);
// ^[a-z0-9][a-z0-9._-]{2,127}$
bool project_id(std::string_view text);
// ^[a-z0-9][a-z0-9-]{0,31}$
bool tag(std::string_view text);
// ^(?!/)(?!.*\.\.)(?!.*\\)(?!.*:)[A-Za-z0-9._/-]+$ (length 1..200 is
// checked by the caller)
bool contained_path(std::string_view text);
} // namespace patterns

} // namespace core::content
