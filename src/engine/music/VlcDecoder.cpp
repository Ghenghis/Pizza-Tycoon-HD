/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/VlcDecoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

#include "engine/SDL_include.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
// In the order vlc.h includes them: each needs the ones before it.
// clang-format off
#include <vlc/libvlc.h>
#include <vlc/libvlc_renderer_discoverer.h>
#include <vlc/libvlc_media.h>
#include <vlc/libvlc_media_player.h>
// clang-format on
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace music {

namespace {

// The output format asked of libVLC: the game's music format.
constexpr unsigned vlc_rate = 44100;
constexpr unsigned vlc_channels = 2;
// What the callback holds for the worker: 1.5 s.
constexpr std::size_t pipe_frames = std::size_t{1} << 16;
// How long the callback waits for room before dropping audio.
constexpr auto pipe_wait = std::chrono::milliseconds(100);

constexpr std::array<const char*, 4> library_names = {
#if defined(_WIN32)
    "libvlc.dll",
#elif defined(__APPLE__)
    "libvlc.dylib",
    "/Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib",
#else
    "libvlc.so.5",
    "libvlc.so",
#endif
    nullptr,
};

} // namespace

class VlcRuntime {
public:
  VlcRuntime() = default;
  VlcRuntime(const VlcRuntime&) = delete;
  VlcRuntime& operator=(const VlcRuntime&) = delete;
  ~VlcRuntime() {
    if (instance && release) {
      release(instance);
    }
    if (lib) {
      SDL_UnloadObject(lib);
    }
  }

  bool load(std::string* error) {
    for (const char* name : library_names) {
      if (!name) {
        break;
      }
      lib = SDL_LoadObject(name);
      if (lib) {
        break;
      }
    }
    if (!lib) {
      return fail(error, "libVLC is not installed");
    }
    const bool all = bind(create, "libvlc_new") &&
                     bind(release, "libvlc_release") &&
                     bind(get_version, "libvlc_get_version") &&
                     bind(media_new_path, "libvlc_media_new_path") &&
                     bind(media_add_option, "libvlc_media_add_option") &&
                     bind(media_release, "libvlc_media_release") &&
                     bind(player_new, "libvlc_media_player_new_from_media") &&
                     bind(player_release, "libvlc_media_player_release") &&
                     bind(player_play, "libvlc_media_player_play") &&
                     bind(player_stop, "libvlc_media_player_stop") &&
                     bind(player_state, "libvlc_media_player_get_state") &&
                     bind(set_callbacks, "libvlc_audio_set_callbacks") &&
                     bind(set_format, "libvlc_audio_set_format");
    if (!all) {
      return fail(error, "libVLC is too old (it lacks " + missing + ")");
    }
    // As Fling-Studio's VlcMusicPlayer ("--no-video", "--quiet",
    // "--no-osd"), plus no interface and nothing looked up on the network.
    // No video output means no window, not even for cover art.
    const std::array<const char*, 5> args = {"--intf=dummy", "--no-video",
                                             "--quiet", "--no-osd",
                                             "--no-metadata-network-access"};
    instance = create(static_cast<int>(args.size()), args.data());
    if (!instance) {
      return fail(error, "libVLC did not start (are its plugins installed?)");
    }
    return true;
  }

  decltype(&::libvlc_new) create = nullptr;
  decltype(&::libvlc_release) release = nullptr;
  decltype(&::libvlc_get_version) get_version = nullptr;
  decltype(&::libvlc_media_new_path) media_new_path = nullptr;
  decltype(&::libvlc_media_add_option) media_add_option = nullptr;
  decltype(&::libvlc_media_release) media_release = nullptr;
  decltype(&::libvlc_media_player_new_from_media) player_new = nullptr;
  decltype(&::libvlc_media_player_release) player_release = nullptr;
  decltype(&::libvlc_media_player_play) player_play = nullptr;
  decltype(&::libvlc_media_player_stop) player_stop = nullptr;
  decltype(&::libvlc_media_player_get_state) player_state = nullptr;
  decltype(&::libvlc_audio_set_callbacks) set_callbacks = nullptr;
  decltype(&::libvlc_audio_set_format) set_format = nullptr;

  SDL_SharedObject* lib = nullptr;
  libvlc_instance_t* instance = nullptr;

private:
  template <typename Fn> bool bind(Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(SDL_LoadFunction(lib, name));
    if (!fn && missing.empty()) {
      missing = name;
    }
    return fn != nullptr;
  }
  static bool fail(std::string* error, std::string why) {
    if (error) {
      *error = std::move(why);
    }
    return false;
  }
  std::string missing;
};

std::shared_ptr<VlcRuntime> load_vlc(std::string* error) {
  static std::mutex mutex;
  static std::weak_ptr<VlcRuntime> shared;
  const std::lock_guard lock(mutex);
  if (auto alive = shared.lock()) {
    return alive;
  }
  auto vlc = std::make_shared<VlcRuntime>();
  if (!vlc->load(error)) {
    return nullptr;
  }
  shared = vlc;
  return vlc;
}

std::string vlc_version(const VlcRuntime& vlc) {
  const char* v = vlc.get_version ? vlc.get_version() : nullptr;
  return v ? v : "";
}

namespace {

// What libVLC's audio thread hands over, until the worker takes it. The
// callbacks get borrowed buffers: they copy them and return. A callback
// waits a little when the worker has not caught up, never for the game.
struct Pipe {
  std::mutex mutex;
  std::condition_variable room;
  std::vector<std::int16_t> samples =
      std::vector<std::int16_t>(pipe_frames * vlc_channels);
  std::size_t head = 0;  // frame
  std::size_t count = 0; // frames
  bool drained = false;
  bool stopping = false;
  std::uint64_t dropped = 0; // frames

  void clear() {
    head = 0;
    count = 0;
    drained = false;
  }
};

void on_play(void* data, const void* samples, unsigned count, int64_t) {
  auto& pipe = *static_cast<Pipe*>(data);
  const auto* in = static_cast<const std::int16_t*>(samples);
  std::unique_lock lock(pipe.mutex);
  std::size_t left = count;
  const auto until = std::chrono::steady_clock::now() + pipe_wait;
  while (left > 0) {
    const bool room = pipe.room.wait_until(
        lock, until, [&] { return pipe.stopping || pipe.count < pipe_frames; });
    if (!room || pipe.stopping) {
      pipe.dropped += left;
      return;
    }
    const std::size_t tail = (pipe.head + pipe.count) % pipe_frames;
    const std::size_t n =
        std::min({left, pipe_frames - pipe.count, pipe_frames - tail});
    std::copy(in, in + n * vlc_channels,
              pipe.samples.begin() +
                  static_cast<std::ptrdiff_t>(tail * vlc_channels));
    pipe.count += n;
    in += n * vlc_channels;
    left -= n;
  }
}

void on_flush(void* data, int64_t) {
  auto& pipe = *static_cast<Pipe*>(data);
  const std::lock_guard lock(pipe.mutex);
  pipe.clear();
  pipe.room.notify_all();
}

void on_drain(void* data) {
  auto& pipe = *static_cast<Pipe*>(data);
  const std::lock_guard lock(pipe.mutex);
  pipe.drained = true;
}

class VlcDecoder final : public TrackDecoder {
public:
  VlcDecoder(std::shared_ptr<VlcRuntime> vlc, std::filesystem::path file,
             const StreamInfo& probed)
      : vlc_(std::move(vlc)), file_(std::move(file)), info_(probed),
        backend_("libVLC " + vlc_version(*vlc_)) {
    // libVLC delivers the format asked for.
    if (probed.sample_rate > 0) {
      info_.frames = probed.frames * vlc_rate / probed.sample_rate;
    }
    info_.sample_rate = vlc_rate;
    info_.channels = vlc_channels;
  }
  ~VlcDecoder() override { stop_player(); }
  VlcDecoder(const VlcDecoder&) = delete;
  VlcDecoder& operator=(const VlcDecoder&) = delete;

  bool start(std::uint64_t frame, std::string* error) {
    const std::string path = utf8_path(file_);
    media_ = vlc_->media_new_path(vlc_->instance, path.c_str());
    if (!media_) {
      return fail(error, "libVLC cannot open the file");
    }
    vlc_->media_add_option(media_, ":no-video");
    if (frame > 0) {
      char option[48];
      std::snprintf(option, sizeof option, ":start-time=%.3f",
                    static_cast<double>(frame) / vlc_rate);
      vlc_->media_add_option(media_, option);
    }
    player_ = vlc_->player_new(media_);
    if (!player_) {
      return fail(error, "libVLC cannot play the file");
    }
    vlc_->set_callbacks(player_, on_play, nullptr, nullptr, on_flush, on_drain,
                        &pipe_);
    vlc_->set_format(player_, "S16N", vlc_rate, vlc_channels);
    if (vlc_->player_play(player_) != 0) {
      return fail(error, "libVLC cannot play the file");
    }
    return true;
  }

  [[nodiscard]] const StreamInfo& info() const override { return info_; }

  std::size_t read(std::span<float> out) override {
    const std::lock_guard lock(pipe_.mutex);
    const std::size_t n = std::min(pipe_.count, out.size() / vlc_channels);
    for (std::size_t i = 0; i < n * vlc_channels; ++i) {
      const std::size_t at =
          (pipe_.head * vlc_channels + i) % (pipe_frames * vlc_channels);
      out[i] = static_cast<float>(pipe_.samples[at]) / 32768.0f;
    }
    pipe_.head = (pipe_.head + n) % pipe_frames;
    pipe_.count -= n;
    if (n > 0) {
      pipe_.room.notify_all();
    }
    return n;
  }

  bool seek(std::uint64_t frame) override {
    stop_player();
    return start(frame, nullptr);
  }

  [[nodiscard]] bool at_end() const override {
    {
      const std::lock_guard lock(pipe_.mutex);
      if (pipe_.count > 0) {
        return false;
      }
      if (pipe_.drained) {
        return true;
      }
    }
    if (!player_) {
      return true;
    }
    const libvlc_state_t state = vlc_->player_state(player_);
    return state == libvlc_Ended || state == libvlc_Error ||
           state == libvlc_Stopped;
  }

  [[nodiscard]] bool damaged() const override {
    return !player_ || vlc_->player_state(player_) == libvlc_Error;
  }

  [[nodiscard]] std::string_view backend() const override { return backend_; }

private:
  // Stops libVLC and waits for its threads, so no callback can run once
  // this returns and the pipe may go.
  void stop_player() {
    {
      const std::lock_guard lock(pipe_.mutex);
      pipe_.stopping = true;
      pipe_.room.notify_all();
    }
    if (player_) {
      vlc_->player_stop(player_);
      vlc_->player_release(player_);
      player_ = nullptr;
    }
    if (media_) {
      vlc_->media_release(media_);
      media_ = nullptr;
    }
    const std::lock_guard lock(pipe_.mutex);
    pipe_.clear();
    pipe_.stopping = false;
  }

  static bool fail(std::string* error, const char* why) {
    if (error) {
      *error = why;
    }
    return false;
  }

  std::shared_ptr<VlcRuntime> vlc_;
  std::filesystem::path file_;
  StreamInfo info_;
  std::string backend_;
  mutable Pipe pipe_;
  libvlc_media_t* media_ = nullptr;
  libvlc_media_player_t* player_ = nullptr;
};

} // namespace

std::unique_ptr<TrackDecoder>
open_vlc_decoder(const std::shared_ptr<VlcRuntime>& vlc,
                 const std::filesystem::path& file, const StreamInfo& probed,
                 std::string* error) {
  if (!vlc) {
    if (error) {
      *error = "libVLC is not available";
    }
    return nullptr;
  }
  auto decoder = std::make_unique<VlcDecoder>(vlc, file, probed);
  if (!decoder->start(0, error)) {
    return nullptr;
  }
  return decoder;
}

std::unique_ptr<TrackDecoder>
open_decoder(const std::filesystem::path& file, const Limits& limits,
             Backend backend, const std::shared_ptr<VlcRuntime>& vlc,
             std::string* error) {
  if (backend == Backend::BUILTIN || (backend == Backend::AUTO && !vlc)) {
    return open_builtin_decoder(file, limits, error);
  }
  // The same header checks first, whichever decoder runs.
  const std::optional<StreamInfo> probed = probe_file(file, limits, error);
  if (!probed) {
    return nullptr;
  }
  std::string why;
  if (auto decoder = open_vlc_decoder(vlc, file, *probed, &why)) {
    return decoder;
  }
  if (backend == Backend::LIBVLC) {
    if (error) {
      *error = why;
    }
    return nullptr;
  }
  return open_builtin_decoder(file, limits, error);
}

} // namespace music
