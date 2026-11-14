/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/JsonShape.h"

#include <algorithm>
#include <set>

#include "core/content/ContentKey.h"

namespace core::content {

namespace {

bool lower_or_digit(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool digit(char c) { return c >= '0' && c <= '9'; }

bool hex_digit(char c) { return digit(c) || (c >= 'a' && c <= 'f'); }

// Code points of a UTF-8 string, or nullopt when it is not valid UTF-8.
std::optional<std::u32string> decode_utf8(std::string_view text) {
  std::u32string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    int extra = 0;
    char32_t cp = 0;
    if (lead < 0x80) {
      cp = lead;
    }
    else if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      cp = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      cp = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      cp = lead & 0x07;
    }
    else {
      return std::nullopt;
    }
    if (extra > 0 && i + static_cast<std::size_t>(extra) >= text.size()) {
      return std::nullopt;
    }
    for (int k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        return std::nullopt;
      }
      cp = (cp << 6) | (next & 0x3F);
    }
    out.push_back(cp);
    i += static_cast<std::size_t>(extra) + 1;
  }
  return out;
}

bool latin1_code_point(char32_t cp) {
  return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF);
}

std::string quote(std::string_view text) {
  std::string out = "'";
  out += text;
  out += "'";
  return out;
}

} // namespace

void JsonShape::fail(const std::string& pointer, std::string message) {
  ++failures_;
  out_.push_back(Finding{.rule = rule_,
                         .severity = Severity::Error,
                         .pointer = pointer,
                         .message = std::move(message)});
}

std::string JsonShape::child(const std::string& pointer, std::string_view key) {
  std::string out = pointer;
  out += '/';
  for (const char c : key) {
    if (c == '~') {
      out += "~0";
    }
    else if (c == '/') {
      out += "~1";
    }
    else {
      out += c;
    }
  }
  return out;
}

std::string JsonShape::child(const std::string& pointer, std::size_t index) {
  return pointer + "/" + std::to_string(index);
}

bool JsonShape::object(const json& value, const std::string& pointer,
                       std::initializer_list<std::string_view> allowed,
                       std::initializer_list<std::string_view> required) {
  if (!value.is_object()) {
    fail(pointer, "expected an object");
    return false;
  }
  bool ok = true;
  for (const std::string_view key : required) {
    if (!value.contains(std::string(key))) {
      fail(child(pointer, key), "missing member " + quote(key));
      ok = false;
    }
  }
  for (const auto& [key, member_value] : value.items()) {
    const bool known =
        std::find(allowed.begin(), allowed.end(), key) != allowed.end() ||
        std::find(required.begin(), required.end(), key) != required.end();
    if (!known) {
      fail(child(pointer, key), "unknown member " + quote(key));
      ok = false;
    }
  }
  return ok;
}

const nlohmann::json* JsonShape::member(const json& object,
                                        std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(std::string(key));
  return it == object.end() ? nullptr : &*it;
}

std::optional<std::int64_t> JsonShape::integer(const json& value,
                                               const std::string& pointer,
                                               std::int64_t minimum,
                                               std::int64_t maximum) {
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(maximum)) {
      fail(pointer, "above maximum " + std::to_string(maximum));
      return std::nullopt;
    }
    number = static_cast<std::int64_t>(raw);
  }
  else if (value.is_number_integer()) {
    number = value.get<std::int64_t>();
  }
  else {
    fail(pointer, "expected an integer");
    return std::nullopt;
  }
  if (number < minimum) {
    fail(pointer, "below minimum " + std::to_string(minimum));
    return std::nullopt;
  }
  if (number > maximum) {
    fail(pointer, "above maximum " + std::to_string(maximum));
    return std::nullopt;
  }
  return number;
}

std::optional<bool> JsonShape::boolean(const json& value,
                                       const std::string& pointer) {
  if (!value.is_boolean()) {
    fail(pointer, "expected a boolean");
    return std::nullopt;
  }
  return value.get<bool>();
}

std::optional<std::string> JsonShape::text(const json& value,
                                           const std::string& pointer,
                                           std::size_t min_length,
                                           std::size_t max_length, bool latin1,
                                           Pattern pattern) {
  if (!value.is_string()) {
    fail(pointer, "expected a string");
    return std::nullopt;
  }
  const auto& text = value.get_ref<const std::string&>();
  const auto code_points = decode_utf8(text);
  if (!code_points) {
    fail(pointer, "not valid UTF-8");
    return std::nullopt;
  }
  if (code_points->size() < min_length) {
    fail(pointer, "shorter than " + std::to_string(min_length));
    return std::nullopt;
  }
  if (code_points->size() > max_length) {
    fail(pointer, "longer than " + std::to_string(max_length));
    return std::nullopt;
  }
  if (latin1 && !std::all_of(code_points->begin(), code_points->end(),
                             latin1_code_point)) {
    fail(pointer, "characters outside ISO-8859-1 text");
    return std::nullopt;
  }
  if (pattern && !pattern(text)) {
    fail(pointer, "does not match the pattern");
    return std::nullopt;
  }
  return text;
}

std::optional<std::size_t>
JsonShape::choice(const json& value, const std::string& pointer,
                  std::initializer_list<std::string_view> options) {
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    std::size_t index = 0;
    for (const std::string_view option : options) {
      if (text == option) {
        return index;
      }
      ++index;
    }
  }
  fail(pointer, "not one of the allowed values");
  return std::nullopt;
}

bool JsonShape::constant(const json& value, const std::string& pointer,
                         std::string_view expected) {
  if (!value.is_string() || value.get_ref<const std::string&>() != expected) {
    fail(pointer, "must be " + quote(expected));
    return false;
  }
  return true;
}

bool JsonShape::constant(const json& value, const std::string& pointer,
                         std::int64_t expected) {
  if (!value.is_number_integer() || value.get<std::int64_t>() != expected) {
    fail(pointer, "must be " + std::to_string(expected));
    return false;
  }
  return true;
}

bool JsonShape::array(const json& value, const std::string& pointer,
                      std::size_t min_items, std::size_t max_items) {
  if (!value.is_array()) {
    fail(pointer, "expected an array");
    return false;
  }
  if (value.size() < min_items) {
    fail(pointer, "fewer than " + std::to_string(min_items) + " items");
    return false;
  }
  if (value.size() > max_items) {
    fail(pointer, "more than " + std::to_string(max_items) + " items");
    return false;
  }
  return true;
}

bool JsonShape::string_set(const json& value, const std::string& pointer,
                           std::size_t max_items, std::size_t min_length,
                           std::size_t max_length, bool latin1,
                           Pattern pattern) {
  if (!array(value, pointer, 0, max_items)) {
    return false;
  }
  bool ok = true;
  std::set<std::string> seen;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto item = text(value[i], child(pointer, i), min_length, max_length,
                           latin1, pattern);
    if (!item) {
      ok = false;
    }
    else if (!seen.insert(*item).second) {
      fail(child(pointer, i), "duplicate item");
      ok = false;
    }
  }
  return ok;
}

std::optional<std::int64_t> JsonShape::integer_at(const json& object,
                                                  const std::string& pointer,
                                                  std::string_view key,
                                                  std::int64_t minimum,
                                                  std::int64_t maximum) {
  const json* value = member(object, key);
  return value ? integer(*value, child(pointer, key), minimum, maximum)
               : std::nullopt;
}

std::optional<bool> JsonShape::boolean_at(const json& object,
                                          const std::string& pointer,
                                          std::string_view key) {
  const json* value = member(object, key);
  return value ? boolean(*value, child(pointer, key)) : std::nullopt;
}

std::optional<std::string>
JsonShape::text_at(const json& object, const std::string& pointer,
                   std::string_view key, std::size_t min_length,
                   std::size_t max_length, bool latin1, Pattern pattern) {
  const json* value = member(object, key);
  return value ? text(*value, child(pointer, key), min_length, max_length,
                      latin1, pattern)
               : std::nullopt;
}

std::optional<std::size_t>
JsonShape::choice_at(const json& object, const std::string& pointer,
                     std::string_view key,
                     std::initializer_list<std::string_view> options) {
  const json* value = member(object, key);
  return value ? choice(*value, child(pointer, key), options) : std::nullopt;
}

namespace patterns {

bool entity_id(std::string_view text) {
  if (text.empty() || text.size() > 48 || text[0] < 'a' || text[0] > 'z') {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(),
                     [](char c) { return lower_or_digit(c) || c == '-'; });
}

bool content_key(std::string_view text) {
  return text.size() <= 96 && ContentKey::is_valid(text);
}

bool sha256(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), hex_digit);
}

bool utc(std::string_view text) {
  // 2026-11-14T00:00:00Z
  constexpr std::string_view shape = "dddd-dd-ddTdd:dd:ddZ";
  if (text.size() != shape.size()) {
    return false;
  }
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (shape[i] == 'd' ? !digit(text[i]) : text[i] != shape[i]) {
      return false;
    }
  }
  return true;
}

namespace {
// 0 or a number without a leading zero, at the start of text; returns the
// length taken or 0.
std::size_t take_number(std::string_view text) {
  if (text.empty() || !digit(text[0])) {
    return 0;
  }
  if (text[0] == '0') {
    return 1;
  }
  std::size_t n = 0;
  while (n < text.size() && digit(text[n])) {
    ++n;
  }
  return n;
}
} // namespace

bool semver(std::string_view text) {
  for (int part = 0; part < 3; ++part) {
    const std::size_t n = take_number(text);
    if (n == 0) {
      return false;
    }
    text.remove_prefix(n);
    if (part < 2) {
      if (text.empty() || text[0] != '.') {
        return false;
      }
      text.remove_prefix(1);
    }
  }
  if (text.empty()) {
    return true;
  }
  if (text[0] != '-' || text.size() < 2) {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(), [](char c) {
    return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '.' || c == '-';
  });
}

bool u64_decimal(std::string_view text) {
  if (text == "0") {
    return true;
  }
  return !text.empty() && text.size() <= 20 && text[0] != '0' &&
         std::all_of(text.begin(), text.end(), digit);
}

bool dotted_id(std::string_view text) {
  if (text.size() < 3 || text.size() > 64 || text[0] < 'a' || text[0] > 'z') {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(), [](char c) {
    return lower_or_digit(c) || c == '.' || c == '-';
  });
}

bool project_id(std::string_view text) {
  if (text.size() < 3 || text.size() > 128 || !lower_or_digit(text[0])) {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(), [](char c) {
    return lower_or_digit(c) || c == '.' || c == '_' || c == '-';
  });
}

bool tag(std::string_view text) {
  if (text.empty() || text.size() > 32 || !lower_or_digit(text[0])) {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(),
                     [](char c) { return lower_or_digit(c) || c == '-'; });
}

bool contained_path(std::string_view text) {
  if (text.empty() || text[0] == '/' ||
      text.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || digit(c) ||
           c == '.' || c == '_' || c == '/' || c == '-';
  });
}

} // namespace patterns

} // namespace core::content
