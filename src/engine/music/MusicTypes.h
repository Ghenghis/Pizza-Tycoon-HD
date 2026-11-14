/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The player's own music (R35): folders of songs, collections, playlists and
// the one owner of what plays as the game's music. See
// docs/studio/notes/music-library.md.
namespace music {

// Stable keys in the studio's namespace:slug form:
//   trk:<32 hex>  the first 128 bits of the SHA-256 of the file's bytes
//   col:<16 hex>  a collection: a folder under a music root
//   pl:<16 hex>   random when the playlist is made, then fixed
using TrackId = std::string;
using CollectionId = std::string;
using PlaylistId = std::string;

// What the music owner plays. Exactly one of them at a time.
enum class Mode : std::uint8_t {
  ORIGINAL,             // the 1994 FM songs, as the game picks them
  CHRISTMAS_BACKGROUND, // the player's music, everywhere it is allowed
  JUKEBOX,              // the player's music, from the North Pole jukebox
  OFF,                  // no music at all; effects play on
};

// Whether the player's music is playing, within a Christmas mode. Pause
// holds the place; stop forgets nothing either, but needs an explicit play.
// Neither is the music mute (AudioManager::music_enabled), under which the
// music plays on silently.
enum class PlayState : std::uint8_t { PLAYING, PAUSED, STOPPED };

// Where AudioManager takes the music block from. Set only by the music
// owner, together with its playback generation.
enum class Source : std::uint8_t {
  ORIGINAL, // the FM player
  MY_MUSIC, // the PCM ring
  OFF,      // no music block
};

enum class Format : std::uint8_t { WAV, MP3, FLAC, OGG_VORBIS };

// Which decoder plays the player's music. AUTO: an installed libVLC when it
// loads, otherwise the built-in decoders (dr_mp3, dr_wav).
enum class Backend : std::uint8_t { AUTO, BUILTIN, LIBVLC };

enum class Repeat : std::uint8_t { OFF, ALL, ONE };

// Limits on what is played, checked from a file's header before any
// decoder runs.
struct Limits {
  std::uint64_t max_file_bytes = std::uint64_t{512} << 20;
  std::uint32_t max_seconds = 2 * 60 * 60;
  // How far into the file the audio may start. ID3v2 tags with cover art
  // come before an MP3's audio, hence the room.
  std::uint64_t max_header_bytes = std::uint64_t{16} << 20;
  std::uint32_t max_channels = 8;
  std::uint32_t min_rate = 8000;
  std::uint32_t max_rate = 384000;
};

// A track to play: what the worker opens, and what notices call it.
struct QueuedTrack {
  TrackId id;
  std::filesystem::path file; // empty when no copy of it was found
  std::string title;
};

// Shown to the player: track changes, skips and the end of playback.
struct Notice {
  enum class Kind : std::uint8_t { TRACK_STARTED, SKIPPED, STOPPED };
  Kind kind = Kind::SKIPPED;
  TrackId track; // empty for STOPPED
  std::string text;
};

inline constexpr std::size_t MAX_TITLE_BYTES = 200;
inline constexpr std::size_t MAX_NAME_BYTES = 100;

// Whether key has the namespace:slug form,
// ^[a-z][a-z0-9-]{1,31}:[a-z0-9][a-z0-9-]{1,63}$.
[[nodiscard]] bool is_valid_key(std::string_view key);
// is_valid_key() and the given namespace, e.g. "trk".
[[nodiscard]] bool is_key_of(std::string_view key, std::string_view ns);

// The track id for a file's SHA-256 in hex: "trk:" and its first 32 digits.
[[nodiscard]] TrackId track_id_for(std::string_view sha256_hex);

// ns:<16 hex> from 64 random bits.
[[nodiscard]] std::string random_key(std::string_view ns);

// text for display: up to the first NUL, as UTF-8 (when it is not valid
// UTF-8 its bytes are read as Latin-1), control characters dropped, spaces
// trimmed, and cut on a character boundary to at most max_bytes.
[[nodiscard]] std::string clean_text(std::string_view text,
                                     std::size_t max_bytes);

// A path as UTF-8, and back.
[[nodiscard]] std::string utf8_path(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

// "wav", "mp3", "flac", "ogg".
[[nodiscard]] std::string_view format_name(Format format);
[[nodiscard]] std::optional<Format> format_from_name(std::string_view name);

[[nodiscard]] std::string_view mode_name(Mode mode);
[[nodiscard]] std::optional<Mode> mode_from_name(std::string_view name);

// A byte count in MiB with one decimal, for messages.
[[nodiscard]] std::string mib(std::uint64_t bytes);

} // namespace music
