/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/record/Recorder.h"

#include <chrono>
#include <cmath>

#include "engine/FormatCompat.h"
#include "engine/Logger.h"

namespace record {

Recorder::~Recorder() {
  if (session_) {
    session_->stop(StopReason::GAME_EXIT);
    session_->wait_finished();
  }
}

const Capabilities& Recorder::capabilities() {
  if (!caps_) {
    caps_ = Capabilities::probe();
  }
  return *caps_;
}

std::string Recorder::start(const Start& options, bool classic) {
  if (session_ && !session_->finished()) {
    return "Already recording: " + describe(session_->status());
  }
  session_.reset();
  if (!classic) {
    return "Recording needs the Classic picture for now (HD off): the HD "
           "path's readback comes later";
  }
  const Capabilities& caps = capabilities();
  Config config;
  config.folder = options.folder;
  config.fps = options.fps;
  config.encoder = options.encoder;
  if (options.seconds) {
    config.max_duration = std::chrono::seconds(
        std::max<long long>(1, std::llround(*options.seconds)));
  }
  std::string why;
  session_ = RecordingSession::start(config, caps, tap_, &why);
  if (!session_) {
    return "Not recording: " + why;
  }
  const Status s = session_->status();
  Logger::info("Recording to {} ({}, {} fps)", s.folder.string(), s.encoder,
               config.fps);
  return compat::format("Recording to {} ({}, {} fps, {})", s.folder.string(),
                        s.encoder, config.fps,
                        options.seconds
                            ? compat::format("{:g} s", *options.seconds)
                            : std::string("until record stop"));
}

std::string Recorder::stop() {
  if (!session_) {
    return last_.empty() ? "Not recording" : "Not recording; " + last_;
  }
  session_->stop(StopReason::USER);
  return "Stopping: " + describe(session_->status());
}

std::string Recorder::status() const {
  const Capabilities* caps = caps_ ? &*caps_ : nullptr;
  std::string text = session_ ? describe(session_->status())
                              : (last_.empty() ? std::string("Not recording")
                                               : "Not recording; " + last_);
  if (caps) {
    text += "\nFFmpeg: " +
            (caps->ffmpeg.empty()
                 ? std::string("not found")
                 : caps->ffmpeg.string() + " (" + caps->version + ")");
    std::string offered;
    for (const std::string& e : caps->offered) {
      offered += (offered.empty() ? "" : ", ") + e;
    }
    std::string present;
    for (const std::string& e : caps->present) {
      present += (present.empty() ? "" : ", ") + e;
    }
    text += "\nEncoders usable: " + (offered.empty() ? "none" : offered) +
            "; listed: " + (present.empty() ? "none" : present);
  }
  return text;
}

bool Recorder::recording() const { return session_ && !session_->finished(); }

bool Recorder::wants_frame() const {
  return session_ && session_->wants_video(Clock::now());
}

void Recorder::frame(const std::uint32_t* pixels, int width, int height,
                     int pitch_bytes) {
  if (session_) {
    static_cast<void>(session_->submit_video(pixels, width, height, pitch_bytes,
                                             Clock::now()));
  }
}

void Recorder::tick() {
  if (session_ && session_->finished()) {
    const Status s = session_->status();
    last_ = describe(s);
    last_ok_ = s.state == State::STOPPED;
    Logger::info("Recording ended: {}", last_);
    session_.reset();
  }
}

std::string Recorder::describe(const Status& s) {
  std::string text = compat::format(
      "{} {:.1f} s, {} frames ({} held, {} dropped), {} MB, {} segment(s) "
      "in {}",
      to_string(s.state), s.elapsed_s, s.frames_written, s.frames_held,
      s.frames_dropped, s.bytes_written >> 20, s.segments.size(),
      s.folder.string());
  if (s.stop_reason != StopReason::NONE) {
    text += compat::format(" (stopped: {})", to_string(s.stop_reason));
  }
  if (!s.error.empty()) {
    text += "; " + s.error;
  }
  return text;
}

} // namespace record
