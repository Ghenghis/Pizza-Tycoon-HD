/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace audio {
class MixTap;
}

// Recording the game's own picture and sound (R34, the first slice of
// docs/studio/notes/recording.md): frames of the composed picture handed
// over by the game thread, the final mix from the audio tap, both to the
// official FFmpeg as short Matroska segments, with a manifest.
namespace record {

using Clock = std::chrono::steady_clock;

// What this computer's FFmpeg can do. Probed once, before a session.
struct Capabilities {
  std::filesystem::path ffmpeg; // empty: none found
  std::string version;          // its first -version line
  // Video encoders it lists, of those the recorder knows (hardware ones
  // included), and those that opened a trial encode here. Only the latter
  // can be chosen; the hardware ones are not tried yet.
  std::vector<std::string> present;
  std::vector<std::string> offered;
  bool aac = false;    // the audio encoder opened a trial encode
  std::string why_not; // when nothing can be recorded

  [[nodiscard]] static Capabilities
  probe(const std::optional<std::filesystem::path>& ffmpeg = {});
};

struct Config {
  // A new folder for the session is made in here ("rec-<date>-<time>").
  std::filesystem::path folder;
  int width = 640; // of the frames handed over; others are scaled to it
  int height = 400;
  int fps = 30;
  std::chrono::seconds max_duration{3600};
  std::uint64_t quota_bytes = std::uint64_t{8} << 30;
  std::uint64_t reserve_bytes = std::uint64_t{1} << 30; // free space kept
  int segment_seconds = 60;
  std::string encoder; // one of Capabilities::offered; empty: the first
  int crf = 18;        // libx264 / libopenh264 quality
  int audio_kbps = 192;
};

enum class StopReason {
  NONE,
  USER,
  DURATION,
  QUOTA,
  DISK_SPACE,
  ENCODER_ERROR,
  GAME_EXIT
};
[[nodiscard]] const char* to_string(StopReason reason);

enum class State { RECORDING, STOPPING, STOPPED, FAILED };
[[nodiscard]] const char* to_string(State state);

struct Segment {
  std::string file; // in the session folder
  double start_s = 0;
  double end_s = 0;
  std::uint64_t bytes = 0;
  std::string sha256;
};

struct Status {
  State state = State::RECORDING;
  StopReason stop_reason = StopReason::NONE;
  std::filesystem::path folder; // the session's own
  std::string encoder;
  double elapsed_s = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t frames_submitted = 0;  // handed over by the game
  std::uint64_t frames_dropped = 0;    // no free slot when handed over
  std::uint64_t frames_written = 0;    // to the encoder, at the fixed rate
  std::uint64_t frames_held = 0;       // of those, repeats of the last one
  std::uint64_t audio_samples = 0;     // frames of stereo written
  std::uint64_t audio_gap_samples = 0; // of those, silence for lost audio
  std::vector<Segment> segments;       // once stopped
  std::optional<int> exit_code;
  std::string error;
};

class RecordingSession {
public:
  // Checks the folder, the free space and the encoder, enables the tap and
  // starts FFmpeg, from the calling thread (on Linux FFmpeg ends with it, so
  // call it from the game thread). Null, with the reason, when any of that
  // fails; nothing is recorded then.
  static std::unique_ptr<RecordingSession> start(const Config& config,
                                                 const Capabilities& caps,
                                                 audio::MixTap& tap,
                                                 std::string* why);
  // Stops (GAME_EXIT) if still recording and waits until the files are
  // finished.
  ~RecordingSession();
  RecordingSession(const RecordingSession&) = delete;
  RecordingSession& operator=(const RecordingSession&) = delete;

  // Game thread: whether a frame is due at now, so the game copies one only
  // at the recording's rate.
  [[nodiscard]] bool wants_video(Clock::time_point now) const;
  // Game thread: copies a frame of 32-bit RGBA (SDL_PIXELFORMAT_RGBA8888:
  // each pixel 0xRRGGBBAA), pitch in bytes, shown at t. Never waits on the
  // encoder. False when no slot was free: the frame is dropped and counted,
  // and the last one is held.
  bool submit_video(const std::uint32_t* pixels, int width, int height,
                    int pitch_bytes, Clock::time_point t);
  // The recorder's audio thread: a run of stereo s16 frames whose first is
  // at tap position first. Positions before the start, or already written,
  // are skipped; a gap is written as silence.
  void submit_audio(std::span<const std::int16_t> samples, std::uint64_t first);

  // Any thread. Ends the recording; the files are finished in the
  // background (status().state becomes STOPPED or FAILED).
  void stop(StopReason reason);
  [[nodiscard]] Status status() const;
  [[nodiscard]] bool finished() const;
  // Blocks until finished (tests and exit).
  void wait_finished();

private:
  RecordingSession() = default;
  void video_loop();
  void audio_loop();
  void control_loop();
  bool write_audio(std::span<const std::int16_t> samples);
  bool write_silence(std::uint64_t frames);
  void fail(std::string error);
  void finish();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace record
