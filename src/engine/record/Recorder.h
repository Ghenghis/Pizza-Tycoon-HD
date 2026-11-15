/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "engine/record/RecordingSession.h"

namespace audio {
class MixTap;
}

// The game's side of recording: one session at a time, started and stopped
// by the console (record start|stop|status), fed a frame by the game loop
// when one is due. Game thread only.
namespace record {

class Recorder {
public:
  explicit Recorder(audio::MixTap& tap) : tap_(tap) {}
  ~Recorder(); // a recording still running is finished first

  struct Start {
    std::filesystem::path folder;  // where the session's folder goes
    std::optional<double> seconds; // stops by itself after this long
    int fps = 30;
    std::string encoder; // empty: the best that works here
  };
  // classic: the frame is the software path's (no GPU readback), the only
  // one recorded in this round. The message says what happened.
  std::string start(const Start& options, bool classic);
  std::string stop();
  [[nodiscard]] std::string status() const;
  [[nodiscard]] bool recording() const;
  // Whether the last session ended with its files complete.
  [[nodiscard]] bool last_succeeded() const { return last_ok_; }

  // Game loop, once a frame: whether to copy this frame for the recording,
  // and the copy (32-bit RGBA as SDL_PIXELFORMAT_RGBA8888).
  [[nodiscard]] bool wants_frame() const;
  void frame(const std::uint32_t* pixels, int width, int height,
             int pitch_bytes);
  // Game loop, once a frame: a finished session is let go, and logged.
  void tick();

  // The probe, made at the first start (it runs FFmpeg a few times).
  [[nodiscard]] const Capabilities& capabilities();

private:
  [[nodiscard]] static std::string describe(const Status& s);

  audio::MixTap& tap_;
  std::optional<Capabilities> caps_;
  std::unique_ptr<RecordingSession> session_;
  std::string last_; // how the last one ended
  bool last_ok_ = false;
};

} // namespace record
