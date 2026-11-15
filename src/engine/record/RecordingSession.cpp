/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/record/RecordingSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

#include "core/content/Sha256.h"
#include "engine/audio/MixTap.h"
#include "engine/record/Subprocess.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace record {

namespace {

constexpr int AUDIO_RATE = 44100;
constexpr std::size_t VIDEO_SLOTS = 4;
constexpr auto PROBE_TIMEOUT = std::chrono::seconds(20);
constexpr auto FINISH_TIMEOUT = std::chrono::seconds(15);
constexpr auto WRITER_TIMEOUT = std::chrono::seconds(10);
constexpr std::size_t MANIFEST_STDERR = 4096;

// The software encoders tried here, best first, and the hardware ones only
// listed (later rounds initialise and offer them).
constexpr std::array<const char*, 3> software_encoders = {
    "libx264", "libopenh264", "mpeg4"};
constexpr std::array<const char*, 10> hardware_encoders = {
    "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_amf", "hevc_amf",
    "av1_amf",    "h264_qsv",   "hevc_qsv",  "h264_mf",  "hevc_mf"};

std::vector<std::string> trial_args(const std::string& encoder) {
  return {"-hide_banner",
          "-nostdin",
          "-loglevel",
          "error",
          "-f",
          "lavfi",
          "-i",
          "color=c=black:s=64x64:r=30:d=0.2",
          "-c:v",
          encoder,
          "-pix_fmt",
          "yuv420p",
          "-f",
          "null",
          "-"};
}

std::vector<std::string> video_codec_args(const std::string& encoder, int crf,
                                          int segment_seconds) {
  // A keyframe at every segment start, so each segment plays on its own.
  const std::string keys =
      "expr:gte(t,n_forced*" + std::to_string(segment_seconds) + ")";
  if (encoder == "libx264") {
    return {"-c:v",     "libx264", "-preset",
            "veryfast", "-crf",    std::to_string(crf),
            "-pix_fmt", "yuv420p", "-force_key_frames",
            keys};
  }
  if (encoder == "libopenh264") {
    return {"-c:v",    "libopenh264",       "-b:v", "8M", "-pix_fmt",
            "yuv420p", "-force_key_frames", keys};
  }
  return {
      "-c:v", encoder, "-q:v", "3", "-pix_fmt", "yuv420p", "-force_key_frames",
      keys};
}

std::string session_name() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "rec-%Y%m%d-%H%M%S", &local);
  return text;
}

std::uint64_t folder_bytes(const fs::path& dir) {
  std::uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::error_code ec2;
    if (e.is_regular_file(ec2)) {
      total += e.file_size(ec2);
    }
  }
  return total;
}

std::string file_sha256(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  core::content::Sha256 sha;
  std::array<char, 1 << 16> buffer{};
  while (in) {
    in.read(buffer.data(), buffer.size());
    sha.update(
        std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
  }
  const auto digest = sha.finish();
  static constexpr char hexdigits[] = "0123456789abcdef";
  std::string hex;
  for (const std::uint8_t b : digest) {
    hex += hexdigits[b >> 4];
    hex += hexdigits[b & 15];
  }
  return hex;
}

} // namespace

const char* to_string(StopReason reason) {
  switch (reason) {
  case StopReason::NONE:
    return "none";
  case StopReason::USER:
    return "user";
  case StopReason::DURATION:
    return "duration";
  case StopReason::QUOTA:
    return "quota";
  case StopReason::DISK_SPACE:
    return "disk-space";
  case StopReason::ENCODER_ERROR:
    return "encoder-error";
  case StopReason::GAME_EXIT:
    break;
  }
  return "game-exit";
}

const char* to_string(State state) {
  switch (state) {
  case State::RECORDING:
    return "recording";
  case State::STOPPING:
    return "stopping";
  case State::STOPPED:
    return "stopped";
  case State::FAILED:
    break;
  }
  return "failed";
}

Capabilities Capabilities::probe(const std::optional<fs::path>& ffmpeg) {
  Capabilities caps;
  std::optional<fs::path> explicit_path = ffmpeg;
  if (!explicit_path) {
    if (const char* env = std::getenv("PLEGACY_FFMPEG"); env && *env) {
      explicit_path = fs::path(env);
    }
  }
  const auto exe = find_program("ffmpeg", explicit_path);
  if (!exe) {
    caps.why_not = "FFmpeg was not found (put it on the PATH, or set "
                   "PLEGACY_FFMPEG to it)";
    return caps;
  }
  caps.ffmpeg = *exe;
  const auto version =
      Subprocess::run(*exe, {"-hide_banner", "-version"}, PROBE_TIMEOUT);
  if (!version || version->exit_code != 0) {
    caps.why_not = "FFmpeg at " + exe->string() + " does not run";
    return caps;
  }
  caps.version = version->text.substr(0, version->text.find('\n'));
  const auto list =
      Subprocess::run(*exe, {"-hide_banner", "-encoders"}, PROBE_TIMEOUT);
  if (!list || list->exit_code != 0) {
    caps.why_not = "FFmpeg does not list its encoders";
    return caps;
  }
  // Lines like " V....D libx264    libx264 H.264 ...".
  std::vector<std::string> listed;
  std::istringstream lines(list->text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words(line);
    std::string flags;
    std::string name;
    if (words >> flags >> name && flags.size() == 6) {
      listed.push_back(name);
    }
  }
  const auto has = [&](const char* name) {
    return std::find(listed.begin(), listed.end(), name) != listed.end();
  };
  for (const char* name : software_encoders) {
    if (!has(name)) {
      continue;
    }
    caps.present.emplace_back(name);
    const auto trial = Subprocess::run(*exe, trial_args(name), PROBE_TIMEOUT);
    if (trial && trial->exit_code == 0) {
      caps.offered.emplace_back(name);
    }
  }
  for (const char* name : hardware_encoders) {
    if (has(name)) {
      caps.present.emplace_back(name);
    }
  }
  const auto aac =
      Subprocess::run(*exe,
                      {"-hide_banner", "-nostdin", "-loglevel", "error", "-f",
                       "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "0.2",
                       "-c:a", "aac", "-f", "null", "-"},
                      PROBE_TIMEOUT);
  caps.aac = aac && aac->exit_code == 0;
  if (caps.offered.empty()) {
    caps.why_not = "FFmpeg has no video encoder this recorder can use";
  }
  else if (!caps.aac) {
    caps.why_not = "FFmpeg's AAC audio encoder does not work";
  }
  return caps;
}

/* The session */

struct RecordingSession::Impl {
  Config config;
  std::string encoder;
  audio::MixTap* tap = nullptr;
  fs::path dir;
  std::string name;
  std::string ffmpeg_version;
  fs::path ffmpeg_path;
  std::unique_ptr<Subprocess> ffmpeg;
  // The recording's time 0 and the tap position there: when FFmpeg has
  // taken the first frame (it may take a second to start), so nothing is
  // lost to its start-up. Set once by the video thread.
  std::atomic<Clock::rep> t0_ticks{0};
  std::atomic<std::uint64_t> p0{0};
  std::atomic<bool> started{false};
  [[nodiscard]] Clock::time_point t0() const {
    return Clock::time_point(Clock::duration(t0_ticks.load()));
  }
  double period_s = 1.0 / 30.0;
  // The duration limit, in both streams' units: neither is written past it.
  std::uint64_t max_frames = 0;
  std::uint64_t max_samples = 0;

  // Stopping: the reason is set once; the length in video frames and audio
  // samples follows from where it stopped.
  std::atomic<StopReason> reason{StopReason::NONE};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> stop_frames{0};
  std::atomic<std::uint64_t> stop_samples{0};
  // Video frames begun and audio samples taken on, under mutex: the stop
  // point is never before them, so neither stream runs past it.
  std::uint64_t committed_frames = 0;
  std::atomic<State> state{State::RECORDING};
  std::atomic<bool> video_done{false};
  std::atomic<bool> broken{false}; // the encoder no longer takes input
  std::atomic<bool> audio_done{false};

  // The frames handed over, in slots allocated at the start.
  struct Slot {
    enum class Use { FREE, FILLING, READY, TAKEN };
    std::vector<std::uint32_t> pixels;
    Clock::time_point t;
    std::uint64_t seq = 0;
    Use use = Use::FREE;
  };
  std::array<Slot, VIDEO_SLOTS> slots;
  std::uint64_t seq = 0;
  mutable std::mutex mutex; // slots, the error and the segments
  std::condition_variable wake;
  std::atomic<std::int64_t> next_capture_ns{0};

  std::atomic<std::uint64_t> submitted{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> held{0};
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint64_t> gap{0};
  std::atomic<std::uint64_t> bytes{0};

  std::string error;
  std::vector<Segment> segments;
  std::optional<int> exit_code;

  std::thread video;
  std::thread audio_thread;
  std::thread control;
};

std::unique_ptr<RecordingSession>
RecordingSession::start(const Config& config, const Capabilities& caps,
                        audio::MixTap& tap, std::string* why) {
  const auto refuse = [why](std::string reason) {
    if (why) {
      *why = std::move(reason);
    }
    return std::unique_ptr<RecordingSession>();
  };
  if (!caps.why_not.empty()) {
    return refuse(caps.why_not);
  }
  const std::string encoder =
      config.encoder.empty() ? caps.offered.front() : config.encoder;
  if (std::find(caps.offered.begin(), caps.offered.end(), encoder) ==
      caps.offered.end()) {
    return refuse("the encoder " + encoder + " is not one that works here");
  }
  if (config.width < 16 || config.height < 16 || config.width % 2 != 0 ||
      config.height % 2 != 0 || config.fps < 1 || config.fps > 120 ||
      config.segment_seconds < 1) {
    return refuse("the recording size, rate or segment length is not usable");
  }
  std::error_code ec;
  fs::create_directories(config.folder, ec);
  if (!fs::is_directory(config.folder, ec)) {
    return refuse("the folder " + config.folder.string() + " cannot be made");
  }
  const auto space = fs::space(config.folder, ec);
  if (!ec && space.available < config.reserve_bytes) {
    return refuse("there is less free space than the reserve");
  }

  std::unique_ptr<RecordingSession> s(new RecordingSession());
  s->impl_ = std::make_unique<Impl>();
  Impl& m = *s->impl_;
  m.config = config;
  m.encoder = encoder;
  m.tap = &tap;
  m.ffmpeg_path = caps.ffmpeg;
  m.ffmpeg_version = caps.version;
  m.period_s = 1.0 / config.fps;
  m.max_frames = static_cast<std::uint64_t>(config.max_duration.count()) *
                 static_cast<std::uint64_t>(config.fps);
  m.max_samples =
      m.max_frames * AUDIO_RATE / static_cast<std::uint64_t>(config.fps);
  m.name = session_name();
  m.dir = config.folder / m.name;
  for (int n = 2; fs::exists(m.dir, ec); ++n) {
    m.dir = config.folder / (m.name + "-" + std::to_string(n));
  }
  if (!fs::create_directory(m.dir, ec)) {
    return refuse("the folder " + m.dir.string() + " cannot be made");
  }
  const std::size_t pixels = static_cast<std::size_t>(config.width) *
                             static_cast<std::size_t>(config.height);
  for (auto& slot : m.slots) {
    slot.pixels.assign(pixels, 0);
  }

  const std::string pattern = (m.dir / (m.name + "-%03d.mkv")).string();
  const std::string list = (m.dir / (m.name + ".segments.csv")).string();
  std::vector<std::string> args = {
      "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
      // The picture, as rgb24 at the recording's fixed rate.
      "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size",
      std::to_string(config.width) + "x" + std::to_string(config.height),
      "-framerate", std::to_string(config.fps), "-thread_queue_size", "64",
      // Raw input needs no probing; probing would wait for a second of
      // audio, which comes at its own pace, and stall the picture.
      "-probesize", "32", "-analyzeduration", "0", "-i", "pipe:0",
      // The game's final mix, by sample position.
      "-f", "s16le", "-ar", std::to_string(AUDIO_RATE), "-ac", "2",
      "-thread_queue_size", "1024", "-probesize", "32", "-analyzeduration", "0",
      "-i", std::string(Subprocess::SECOND_INPUT), "-map", "0:v:0", "-map",
      "1:a:0"};
  for (std::string& a :
       video_codec_args(encoder, config.crf, config.segment_seconds)) {
    args.push_back(std::move(a));
  }
  for (std::string a :
       {std::string("-c:a"), std::string("aac"), std::string("-b:a"),
        std::to_string(config.audio_kbps) + "k", std::string("-f"),
        std::string("segment"), std::string("-segment_time"),
        std::to_string(config.segment_seconds), std::string("-segment_format"),
        std::string("matroska"), std::string("-reset_timestamps"),
        std::string("0"), std::string("-segment_list"), list,
        std::string("-segment_list_type"), std::string("csv"), pattern}) {
    args.push_back(std::move(a));
  }

  std::string reason;
  m.ffmpeg = Subprocess::start(caps.ffmpeg, args, &reason);
  if (!m.ffmpeg) {
    return refuse("FFmpeg did not start: " + reason);
  }
  // Audio from now on: what the tap held before is not this recording's.
  tap.enabled(true);
  tap.clear();
  m.p0 = tap.position();
  m.t0_ticks = Clock::now().time_since_epoch().count();
  RecordingSession* raw = s.get();
  m.video = std::thread([raw] { raw->video_loop(); });
  m.audio_thread = std::thread([raw] { raw->audio_loop(); });
  m.control = std::thread([raw] { raw->control_loop(); });
  return s;
}

RecordingSession::~RecordingSession() {
  if (!impl_) {
    return;
  }
  stop(StopReason::GAME_EXIT);
  wait_finished();
}

bool RecordingSession::wants_video(Clock::time_point now) const {
  const Impl& m = *impl_;
  if (m.stopping.load()) {
    return false;
  }
  const auto at =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - m.t0())
          .count();
  return at >= m.next_capture_ns.load(std::memory_order_relaxed);
}

bool RecordingSession::submit_video(const std::uint32_t* pixels, int width,
                                    int height, int pitch_bytes,
                                    Clock::time_point t) {
  Impl& m = *impl_;
  if (m.stopping.load() || !pixels || width <= 0 || height <= 0) {
    return false;
  }
  m.submitted.fetch_add(1, std::memory_order_relaxed);
  // Frame k takes the game as shown within half a period of k periods: the
  // next copy is wanted from the start of the next such window.
  const double at = std::chrono::duration<double>(t - m.t0()).count();
  const double window = std::floor(at / m.period_s + 0.5);
  m.next_capture_ns.store(
      static_cast<std::int64_t>(((window + 0.5) * m.period_s) * 1e9),
      std::memory_order_relaxed);

  Impl::Slot* slot = nullptr;
  {
    const std::lock_guard lock(m.mutex);
    for (auto& s : m.slots) {
      if (s.use == Impl::Slot::Use::FREE) {
        slot = &s;
        break;
      }
    }
    if (!slot) {
      // A newer frame replaces the oldest one not yet taken.
      for (auto& s : m.slots) {
        if (s.use == Impl::Slot::Use::READY && (!slot || s.seq < slot->seq)) {
          slot = &s;
        }
      }
    }
    if (!slot) {
      m.dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slot->use = Impl::Slot::Use::FILLING;
  }
  // The copy, outside the lock; another size is scaled (nearest) to fit.
  const int w = m.config.width;
  const int h = m.config.height;
  const auto* base = reinterpret_cast<const std::uint8_t*>(pixels);
  for (int y = 0; y < h; ++y) {
    const int sy = height == h ? y : y * height / h;
    const auto* row = reinterpret_cast<const std::uint32_t*>(
        base + static_cast<std::ptrdiff_t>(sy) * pitch_bytes);
    std::uint32_t* out = slot->pixels.data() + static_cast<std::size_t>(y) *
                                                   static_cast<std::size_t>(w);
    if (width == w) {
      std::copy_n(row, w, out);
    }
    else {
      for (int x = 0; x < w; ++x) {
        out[x] = row[x * width / w];
      }
    }
  }
  {
    const std::lock_guard lock(m.mutex);
    slot->t = t;
    slot->seq = ++m.seq;
    slot->use = Impl::Slot::Use::READY;
  }
  m.wake.notify_all();
  return true;
}

void RecordingSession::video_loop() {
  Impl& m = *impl_;
  const std::size_t w = static_cast<std::size_t>(m.config.width);
  const std::size_t h = static_cast<std::size_t>(m.config.height);
  std::vector<std::uint8_t> rgb(w * h * 3, 0); // black until the first frame
  for (std::uint64_t k = 0;; ++k) {
    // Frame k shows the game at k periods; it is taken half a period
    // later, so a frame shown just after that moment is still in time.
    const auto due =
        m.t0() +
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(k) * m.period_s));
    const auto half = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(m.period_s / 2));
    Impl::Slot* take = nullptr;
    {
      std::unique_lock lock(m.mutex);
      const auto over = [&] {
        return k >= m.max_frames ||
               (m.stopping.load() && k >= m.stop_frames.load());
      };
      while (!over() && Clock::now() < due + half) {
        m.wake.wait_until(lock, due + half);
      }
      if (over()) {
        break;
      }
      m.committed_frames = k + 1;
      // The newest frame shown by then; older ones waiting are no longer
      // needed, newer ones are for the next.
      for (auto& s : m.slots) {
        if (s.use == Impl::Slot::Use::READY && s.t <= due + half &&
            (!take || s.seq > take->seq)) {
          take = &s;
        }
      }
      if (take) {
        for (auto& s : m.slots) {
          if (s.use == Impl::Slot::Use::READY && s.seq < take->seq) {
            s.use = Impl::Slot::Use::FREE;
          }
        }
        take->use = Impl::Slot::Use::TAKEN;
      }
    }
    if (take) {
      for (std::size_t i = 0; i < w * h; ++i) {
        const std::uint32_t v = take->pixels[i];
        rgb[i * 3] = static_cast<std::uint8_t>(v >> 24);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(v >> 16);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>(v >> 8);
      }
      const std::lock_guard lock(m.mutex);
      take->use = Impl::Slot::Use::FREE;
    }
    else {
      m.held.fetch_add(1, std::memory_order_relaxed);
    }
    if (!m.ffmpeg->write(Subprocess::Input::FIRST, rgb)) {
      fail("FFmpeg stopped reading the picture");
      break;
    }
    m.written.fetch_add(1, std::memory_order_relaxed);
    if (k == 0) {
      // FFmpeg is ready: time 0 is now, and the sound starts here too.
      m.p0 = m.tap->position();
      m.t0_ticks = Clock::now().time_since_epoch().count();
      m.next_capture_ns.store(static_cast<std::int64_t>(m.period_s * 0.5e9));
      m.started = true;
    }
  }
  m.ffmpeg->close(Subprocess::Input::FIRST);
  m.video_done = true;
}

bool RecordingSession::write_audio(std::span<const std::int16_t> samples) {
  Impl& m = *impl_;
  std::size_t frames = samples.size() / 2;
  {
    // Taken on before the write, so a stop meanwhile counts them.
    const std::lock_guard lock(m.mutex);
    const std::uint64_t cap =
        m.stopping.load() ? m.stop_samples.load() : m.max_samples;
    const std::uint64_t done = m.samples.load();
    frames = done >= cap ? 0
                         : static_cast<std::size_t>(
                               std::min<std::uint64_t>(frames, cap - done));
    m.samples.fetch_add(frames, std::memory_order_relaxed);
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples.data());
  if (!m.ffmpeg->write(Subprocess::Input::SECOND,
                       std::span(bytes, frames * 2 * sizeof(std::int16_t)))) {
    fail("FFmpeg stopped reading the sound");
    return false;
  }
  return true;
}

bool RecordingSession::write_silence(std::uint64_t frames) {
  static const std::array<std::int16_t, 4096 * 2> zeros{};
  Impl& m = *impl_;
  m.gap.fetch_add(frames, std::memory_order_relaxed);
  while (frames > 0) {
    const auto n = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, zeros.size() / 2));
    if (!write_audio(std::span(zeros.data(), n * 2))) {
      return false;
    }
    frames -= n;
  }
  return true;
}

void RecordingSession::submit_audio(std::span<const std::int16_t> samples,
                                    std::uint64_t first) {
  Impl& m = *impl_;
  const std::uint64_t frames = samples.size() / 2;
  if (m.broken.load() || first + frames <= m.p0) {
    return; // from before the start
  }
  // Where the run starts in the recording, in samples.
  const std::uint64_t lead = first < m.p0 ? m.p0 - first : 0;
  const std::uint64_t at = first + lead - m.p0;
  const std::uint64_t end = at + (frames - lead);
  std::uint64_t done = m.samples.load();
  if (at > done) {
    if (!write_silence(at - done)) {
      return; // lost audio is silence in its place
    }
    done = at;
  }
  if (end <= done) {
    return; // written already (as silence while it was late)
  }
  const std::uint64_t count = end - done; // write_audio() keeps to the stop
  const std::uint64_t skip = lead + (done - at);
  if (count > 0) {
    static_cast<void>(
        write_audio(samples.subspan(static_cast<std::size_t>(skip * 2),
                                    static_cast<std::size_t>(count * 2))));
  }
}

void RecordingSession::audio_loop() {
  Impl& m = *impl_;
  std::vector<std::int16_t> buffer(4096 * 2);
  Clock::time_point stopped_at{};
  for (;;) {
    if (m.broken.load()) {
      break;
    }
    if (!m.started.load()) {
      if (m.stopping.load() && m.stop_samples.load() == 0) {
        break; // stopped before anything was recorded
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (m.stopping.load()) {
      if (stopped_at == Clock::time_point{}) {
        stopped_at = Clock::now();
      }
      if (m.samples.load() >= m.stop_samples.load()) {
        break;
      }
    }
    if (!m.stopping.load() && m.samples.load() >= m.max_samples) {
      // At the duration limit: the stop is on its way.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    std::uint64_t first = 0;
    const std::size_t got = m.tap->read(buffer, &first);
    if (got > 0) {
      submit_audio(std::span(buffer.data(), got * 2), first);
      continue;
    }
    // No audio: the game made none (no device, or it is busy). Keep up with
    // the clock in silence, so the picture never waits on it.
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - m.t0()).count();
    std::uint64_t expected = static_cast<std::uint64_t>(elapsed * AUDIO_RATE);
    if (m.stopping.load()) {
      expected = std::min(expected, m.stop_samples.load());
      if (Clock::now() - stopped_at > std::chrono::milliseconds(500)) {
        expected = m.stop_samples.load(); // the rest is not coming
      }
    }
    const std::uint64_t done = m.samples.load();
    // Silence only for audio that is clearly not coming: half a second
    // late, or, at the stop, the end after waiting for it.
    const bool given_up =
        m.stopping.load() &&
        Clock::now() - stopped_at > std::chrono::milliseconds(500);
    const std::uint64_t slack =
        given_up ? 0 : static_cast<std::uint64_t>(AUDIO_RATE / 2);
    if (expected > done + slack) {
      if (!write_silence(expected - done - slack / 5)) {
        break;
      }
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  m.ffmpeg->close(Subprocess::Input::SECOND);
  m.audio_done = true;
}

void RecordingSession::fail(std::string error) {
  Impl& m = *impl_;
  m.broken = true;
  {
    const std::lock_guard lock(m.mutex);
    if (m.error.empty()) {
      m.error = std::move(error);
    }
  }
  stop(StopReason::ENCODER_ERROR);
}

void RecordingSession::stop(StopReason reason) {
  Impl& m = *impl_;
  StopReason none = StopReason::NONE;
  if (!m.reason.compare_exchange_strong(none, reason)) {
    return;
  }
  const double elapsed =
      std::min(std::chrono::duration<double>(Clock::now() - m.t0()).count(),
               static_cast<double>(m.config.max_duration.count()));
  {
    // Both streams get exactly the same length, at least what either has
    // begun already.
    const std::lock_guard lock(m.mutex);
    const auto fps = static_cast<std::uint64_t>(m.config.fps);
    const std::uint64_t audio_frames =
        (m.samples.load() * fps + AUDIO_RATE - 1) / AUDIO_RATE;
    const std::uint64_t frames = std::min(
        m.max_frames, std::max({static_cast<std::uint64_t>(
                                    std::llround(elapsed * m.config.fps)),
                                m.committed_frames, audio_frames}));
    m.stop_frames = frames;
    m.stop_samples = (frames * AUDIO_RATE + fps / 2) / fps;
    m.stopping = true;
  }
  State expected = State::RECORDING;
  m.state.compare_exchange_strong(expected, State::STOPPING);
  m.wake.notify_all();
}

void RecordingSession::control_loop() {
  Impl& m = *impl_;
  auto last_bytes_check = Clock::now();
  while (!m.stopping.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto now = Clock::now();
    if (m.started.load() && now - m.t0() >= m.config.max_duration) {
      stop(StopReason::DURATION);
      break;
    }
    if (const auto code = m.ffmpeg->wait(std::chrono::milliseconds(0))) {
      fail("FFmpeg ended by itself (" + std::to_string(*code) +
           "): " + m.ffmpeg->stderr_tail());
      break;
    }
    if (now - last_bytes_check >= std::chrono::milliseconds(500)) {
      last_bytes_check = now;
      m.bytes = folder_bytes(m.dir);
      std::error_code ec;
      const auto space = fs::space(m.dir, ec);
      if (m.bytes.load() >= m.config.quota_bytes) {
        stop(StopReason::QUOTA);
      }
      else if (!ec && space.available < m.config.reserve_bytes) {
        stop(StopReason::DISK_SPACE);
      }
    }
  }
  // The writers end at the stop; a stuck encoder is killed to free them.
  const auto until = Clock::now() + WRITER_TIMEOUT;
  while ((!m.video_done.load() || !m.audio_done.load()) &&
         Clock::now() < until) {
    m.wake.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!m.video_done.load() || !m.audio_done.load()) {
    m.ffmpeg->kill();
  }
  m.video.join();
  m.audio_thread.join();
  m.tap->enabled(false);
  auto code = m.ffmpeg->wait(FINISH_TIMEOUT);
  if (!code) {
    m.ffmpeg->kill();
    code = m.ffmpeg->wait(FINISH_TIMEOUT);
  }
  {
    const std::lock_guard lock(m.mutex);
    m.exit_code = code;
    if (code && *code != 0 && m.error.empty()) {
      m.error = "FFmpeg ended with " + std::to_string(*code) + ": " +
                m.ffmpeg->stderr_tail();
    }
  }
  finish();
}

void RecordingSession::finish() {
  Impl& m = *impl_;
  // The segments FFmpeg finished, from its list: "file,start,end".
  std::vector<Segment> segments;
  std::ifstream list(m.dir / (m.name + ".segments.csv"));
  std::string line;
  while (std::getline(list, line)) {
    std::istringstream fields(line);
    std::string file;
    std::string start;
    std::string end;
    if (!std::getline(fields, file, ',') || !std::getline(fields, start, ',') ||
        !std::getline(fields, end, ',')) {
      continue;
    }
    Segment seg;
    seg.file = file;
    seg.start_s = std::strtod(start.c_str(), nullptr);
    seg.end_s = std::strtod(end.c_str(), nullptr);
    std::error_code ec;
    seg.bytes = fs::file_size(m.dir / file, ec);
    seg.sha256 = file_sha256(m.dir / file);
    segments.push_back(std::move(seg));
  }
  m.bytes = folder_bytes(m.dir);

  std::string error;
  std::optional<int> exit_code;
  {
    const std::lock_guard lock(m.mutex);
    m.segments = segments;
    error = m.error;
    exit_code = m.exit_code;
  }
  const bool ok = error.empty() && exit_code && *exit_code == 0;
  json segs = json::array();
  for (const Segment& s : segments) {
    segs.push_back({{"file", s.file},
                    {"start_s", s.start_s},
                    {"end_s", s.end_s},
                    {"bytes", s.bytes},
                    {"sha256", s.sha256}});
  }
  std::string tail = m.ffmpeg->stderr_tail();
  if (tail.size() > MANIFEST_STDERR) {
    tail.erase(0, tail.size() - MANIFEST_STDERR);
  }
  const auto frames = m.written.load();
  const json manifest = {
      {"format", "pthd-recording"},
      {"version", 1},
      {"name", m.name},
      {"complete", ok},
      {"stop_reason", to_string(m.reason.load())},
      {"error", error},
      {"exit_code", exit_code ? json(*exit_code) : json(nullptr)},
      {"duration_s", static_cast<double>(frames) / m.config.fps},
      {"video",
       {{"encoder", m.encoder},
        {"width", m.config.width},
        {"height", m.config.height},
        {"fps", m.config.fps},
        {"frames", frames},
        {"held", m.held.load()},
        {"submitted", m.submitted.load()},
        {"dropped", m.dropped.load()}}},
      {"audio",
       {{"codec", "aac"},
        {"kbps", m.config.audio_kbps},
        {"rate", AUDIO_RATE},
        {"channels", 2},
        {"samples", m.samples.load()},
        {"gap_samples", m.gap.load()},
        {"source", "final game mix, by sample position"}}},
      {"segment_seconds", m.config.segment_seconds},
      {"segments", segs},
      {"ffmpeg",
       {{"path", m.ffmpeg_path.string()}, {"version", m.ffmpeg_version}}},
      {"ffmpeg_stderr_tail", tail}};
  std::ofstream out(m.dir / "recording.json", std::ios::binary);
  out << manifest.dump(2) << "\n";
  m.state = ok ? State::STOPPED : State::FAILED;
  m.wake.notify_all();
}

Status RecordingSession::status() const {
  const Impl& m = *impl_;
  Status s;
  s.state = m.state.load();
  s.stop_reason = m.reason.load();
  s.folder = m.dir;
  s.encoder = m.encoder;
  s.elapsed_s =
      m.stopping.load()
          ? static_cast<double>(m.stop_frames.load()) / m.config.fps
          : std::chrono::duration<double>(Clock::now() - m.t0()).count();
  s.bytes_written = m.bytes.load();
  s.frames_submitted = m.submitted.load();
  s.frames_dropped = m.dropped.load();
  s.frames_written = m.written.load();
  s.frames_held = m.held.load();
  s.audio_samples = m.samples.load();
  s.audio_gap_samples = m.gap.load();
  const std::lock_guard lock(m.mutex);
  s.segments = m.segments;
  s.exit_code = m.exit_code;
  s.error = m.error;
  return s;
}

bool RecordingSession::finished() const {
  const State s = impl_->state.load();
  return s == State::STOPPED || s == State::FAILED;
}

void RecordingSession::wait_finished() {
  if (impl_->control.joinable()) {
    impl_->control.join();
  }
}

} // namespace record
