/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core::content {

// The stable name of a piece of content, "<namespace>:<slug>", for example
// "classic:paris" (docs/studio/location-contract.md section 2.1). It never
// comes from a display name, so renaming a place keeps its key. Indices such
// as CityId are separate and belong to one game.
//
// namespace: a-z first, then a-z, 0-9 or '-', 2..32 characters.
// slug: a-z or 0-9 first, then a-z, 0-9 or '-', 2..64 characters.
class ContentKey {
public:
  static constexpr std::size_t min_part_length = 2;
  static constexpr std::size_t max_namespace_length = 32;
  static constexpr std::size_t max_slug_length = 64;

  [[nodiscard]] static constexpr bool is_valid(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    const std::string_view ns = text.substr(0, colon);
    const std::string_view slug = text.substr(colon + 1);
    return valid_part(ns, max_namespace_length, /*digit_first=*/false) &&
           valid_part(slug, max_slug_length, /*digit_first=*/true);
  }

  [[nodiscard]] static std::optional<ContentKey> parse(std::string_view text) {
    if (!is_valid(text)) {
      return std::nullopt;
    }
    return ContentKey(std::string(text));
  }

  [[nodiscard]] const std::string& str() const { return text_; }

  [[nodiscard]] std::string_view namespace_part() const {
    const std::string_view text = text_;
    return text.substr(0, text.find(':'));
  }

  [[nodiscard]] std::string_view slug() const {
    const std::string_view text = text_;
    return text.substr(text.find(':') + 1);
  }

  [[nodiscard]] bool in_namespace(std::string_view ns) const {
    return namespace_part() == ns;
  }

  friend bool operator==(const ContentKey&, const ContentKey&) = default;
  friend std::strong_ordering operator<=>(const ContentKey& a,
                                          const ContentKey& b) {
    return a.text_.compare(b.text_) <=> 0;
  }

private:
  explicit ContentKey(std::string text) : text_(std::move(text)) {}

  static constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
  static constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

  static constexpr bool valid_part(std::string_view part,
                                   std::size_t max_length, bool digit_first) {
    if (part.size() < min_part_length || part.size() > max_length) {
      return false;
    }
    const char first = part.front();
    if (!is_lower(first) && !(digit_first && is_digit(first))) {
      return false;
    }
    for (const char c : part.substr(1)) {
      if (!is_lower(c) && !is_digit(c) && c != '-') {
        return false;
      }
    }
    return true;
  }

  std::string text_;
};

// Namespaces with a fixed meaning (location-contract.md section 2.1).
inline constexpr std::string_view classic_namespace = "classic";
inline constexpr std::string_view first_party_namespace = "pthd";
// Synthetic catalog entries for UI tests; never built into a game.
inline constexpr std::string_view qa_namespace = "qa-synthetic";

} // namespace core::content

namespace std {
template <> struct hash<core::content::ContentKey> {
  std::size_t operator()(const core::content::ContentKey& key) const noexcept {
    return std::hash<std::string>{}(key.str());
  }
};
} // namespace std
