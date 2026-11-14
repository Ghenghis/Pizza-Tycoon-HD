/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "engine/music/TrackDecoder.h"

namespace music {

class VlcRuntime;

// An installed libVLC, loaded when first asked for (SDL_LoadObject: there is
// no link-time dependency). Shared by the decoders that use it, and unloaded
// when the last of them lets go. Worker thread only.
//
// libVLC's own audio output is replaced by callbacks
// (libvlc_audio_set_callbacks, S16N stereo at 44100 Hz), so its decoded
// audio goes through the game's ring and mixer like any other music, and a
// recording of the game's mix includes it. Video output is turned off, so no
// window can open, not even for cover art.
std::shared_ptr<VlcRuntime> load_vlc(std::string* error);

// Opens file through libVLC; probed is what read_stream_info() found, after
// it checked the file against the limits. null, with the reason, when
// libVLC is not there or will not open it.
[[nodiscard]] std::unique_ptr<TrackDecoder>
open_vlc_decoder(const std::shared_ptr<VlcRuntime>& vlc,
                 const std::filesystem::path& file, const StreamInfo& probed,
                 std::string* error);

// The libVLC version, e.g. "3.0.20 Vetinari"; empty when not loaded.
[[nodiscard]] std::string vlc_version(const VlcRuntime& vlc);

// Opens file with the backend asked for. AUTO tries libVLC when vlc is set,
// then the built-in decoders. The header and limits are checked first,
// whichever decoder runs.
[[nodiscard]] std::unique_ptr<TrackDecoder>
open_decoder(const std::filesystem::path& file, const Limits& limits,
             Backend backend, const std::shared_ptr<VlcRuntime>& vlc,
             std::string* error);

} // namespace music
