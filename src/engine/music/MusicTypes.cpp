/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/MusicTypes.h"

#include <array>
#include <cstdio>
#include <random>
#include <utfcpp/utf8.h>

namespace music {

namespace {

bool slug_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

constexpr std::array<std::string_view, 4> format_names = {"wav", "mp3", "flac",
                                                          "ogg"};
constexpr std::array<std::string_view, 4> mode_names = {
    "original", "christmas-background", "jukebox", "off"};

// Appends one Latin-1 byte as UTF-8.
void append_latin1(std::string& out, unsigned char c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  }
  else {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

} // namespace

bool is_valid_key(std::string_view key) {
  const std::size_t colon = key.find(':');
  if (colon == std::string_view::npos || colon < 2 || colon > 32) {
    return false;
  }
  const std::string_view ns = key.substr(0, colon);
  const std::string_view slug = key.substr(colon + 1);
  if (ns[0] < 'a' || ns[0] > 'z' || slug.size() < 2 || slug.size() > 64 ||
      slug[0] == '-') {
    return false;
  }
  for (const char c : ns) {
    if (!slug_char(c)) {
      return false;
    }
  }
  for (const char c : slug) {
    if (!slug_char(c)) {
      return false;
    }
  }
  return true;
}

bool is_key_of(std::string_view key, std::string_view ns) {
  return key.size() > ns.size() && key.substr(0, ns.size()) == ns &&
         key[ns.size()] == ':' && is_valid_key(key);
}

TrackId track_id_for(std::string_view sha256_hex) {
  return "trk:" + std::string(sha256_hex.substr(0, 32));
}

std::string random_key(std::string_view ns) {
  static thread_local std::mt19937_64 rng{[] {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }()};
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(rng()));
  return std::string(ns) + ":" + hex;
}

std::string clean_text(std::string_view text, std::size_t max_bytes) {
  text = text.substr(0, text.find('\0'));
  std::string utf8;
  if (utf8::is_valid(text)) {
    utf8.assign(text);
  }
  else {
    for (const char c : text) {
      append_latin1(utf8, static_cast<unsigned char>(c));
    }
  }
  std::string out;
  out.reserve(utf8.size());
  for (const char c : utf8) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7F) {
      out.push_back(c);
    }
    else if (u == '\t' || u == '\n' || u == '\r') {
      out.push_back(' ');
    }
  }
  const std::size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  out.erase(0, first);
  out.erase(out.find_last_not_of(' ') + 1);
  if (out.size() > max_bytes) {
    std::size_t cut = max_bytes;
    // Back off to the start of a character.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out.resize(cut);
    out.erase(out.find_last_not_of(' ') + 1);
  }
  return out;
}

std::string utf8_path(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(std::string_view text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string_view format_name(Format format) {
  return format_names[static_cast<std::size_t>(format)];
}

std::optional<Format> format_from_name(std::string_view name) {
  for (std::size_t i = 0; i < format_names.size(); ++i) {
    if (format_names[i] == name) {
      return static_cast<Format>(i);
    }
  }
  return std::nullopt;
}

std::string_view mode_name(Mode mode) {
  return mode_names[static_cast<std::size_t>(mode)];
}

std::optional<Mode> mode_from_name(std::string_view name) {
  for (std::size_t i = 0; i < mode_names.size(); ++i) {
    if (mode_names[i] == name) {
      return static_cast<Mode>(i);
    }
  }
  return std::nullopt;
}

std::string mib(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f MiB",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
  return text;
}

} // namespace music
