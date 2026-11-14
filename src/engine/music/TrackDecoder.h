/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/music/AudioHeaders.h"
#include "engine/music/MusicTypes.h"

struct SDL_AudioStream;

namespace music {

// One way of decoding a track: the built-in decoders (dr_wav, dr_mp3) or an
// installed libVLC. The music worker is the only thread that calls one.
class TrackDecoder {
public:
  virtual ~TrackDecoder() = default;

  // The stream as its header describes it; sample_rate and channels are
  // what read() delivers.
  [[nodiscard]] virtual const StreamInfo& info() const = 0;
  // Reads up to out.size() / channels frames, interleaved float at the
  // source rate. 0 means nothing for now: at_end() tells whether more will
  // come.
  virtual std::size_t read(std::span<float> out) = 0;
  // Moves to frame (at the source rate). False when it cannot.
  virtual bool seek(std::uint64_t frame) = 0;
  [[nodiscard]] virtual bool at_end() const = 0;
  // The track ended or failed before its header said it would.
  [[nodiscard]] virtual bool damaged() const = 0;
  // "dr_mp3 0.7.4", "libVLC 3.0.20": for the advanced settings.
  [[nodiscard]] virtual std::string_view backend() const = 0;
};

// Opens file with the built-in decoders. Its size, header and duration are
// checked against limits (read_stream_info()) before a decoder runs.
// Returns null, with the reason for the player in *error, when the file is
// refused or the decoder cannot read it.
[[nodiscard]] std::unique_ptr<TrackDecoder>
open_builtin_decoder(const std::filesystem::path& file, const Limits& limits,
                     std::string* error);

// Reads the file's header and tags only (see read_stream_info()).
[[nodiscard]] std::optional<StreamInfo>
probe_file(const std::filesystem::path& file, const Limits& limits,
           std::string* error);

// A decoder's output converted to the game's music format, int16 stereo at
// OUTPUT_RATE, through an SDL_AudioStream that is never bound to a device.
// Mono plays on both sides; more channels are mixed down by SDL. Worker
// thread only.
class TrackReader {
public:
  static constexpr int OUTPUT_RATE = 44100;

  TrackReader() = default;
  ~TrackReader();
  TrackReader(const TrackReader&) = delete;
  TrackReader& operator=(const TrackReader&) = delete;

  // Takes over decoder. False, with the reason in *error, when SDL cannot
  // convert its format.
  bool open(std::unique_ptr<TrackDecoder> decoder, std::string* error);
  void close();
  [[nodiscard]] bool is_open() const { return decoder_ != nullptr; }

  // Fills out with whole output frames and returns how many. 0 with
  // !at_end(): nothing for now.
  std::size_t read(std::span<std::int16_t> out);
  // Moves to an output frame. False when the decoder cannot seek.
  bool seek(std::uint64_t output_frame);
  // Everything was read.
  [[nodiscard]] bool at_end() const { return finished_; }
  [[nodiscard]] bool damaged() const;
  // Output frames from the start of the track to the next one read.
  [[nodiscard]] std::uint64_t position() const { return position_; }
  // The track's length in output frames; 0 when not known.
  [[nodiscard]] std::uint64_t duration() const;
  [[nodiscard]] const StreamInfo& info() const;
  [[nodiscard]] std::string_view backend() const;

private:
  std::unique_ptr<TrackDecoder> decoder_;
  SDL_AudioStream* stream_ = nullptr;
  std::vector<float> input_;
  std::uint64_t position_ = 0;
  bool flushed_ = false;
  bool finished_ = false;
  bool failed_ = false;
};

} // namespace music
