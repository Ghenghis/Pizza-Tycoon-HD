/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "engine/music/MusicTypes.h"

struct SDL_IOStream;

namespace music {

// What a file's header and tags say, read by our own bounded readers before
// any decoder sees the file. Tags are never taken from a decoder.
struct StreamInfo {
  Format format = Format::WAV;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint64_t frames = 0;       // at sample_rate
  bool frames_exact = false;      // false: estimated from the bitrate
  std::uint32_t bitrate_kbps = 0; // 0 when not known
  std::string encoding;           // e.g. "PCM 16-bit", "MPEG-1 Layer III"
  // Valid UTF-8, at most MAX_TITLE_BYTES; empty when the file has none.
  std::string title;
  std::string artist;
  std::string album;
};

// The format the first bytes of a file are in (at least 12 of them), or
// nullopt when it is none we know.
[[nodiscard]] std::optional<Format>
sniff_format(std::span<const std::uint8_t> head);

// Reads the header and tags of the file in io, file_bytes long, and checks
// them against limits: size, channels, rate, duration and how far in the
// audio starts. Reads a bounded number of bytes whatever the file says.
// Returns nullopt, with the reason for the player in *error, when the file
// is refused.
[[nodiscard]] std::optional<StreamInfo>
read_stream_info(SDL_IOStream* io, std::uint64_t file_bytes,
                 const Limits& limits, std::string* error);

} // namespace music
