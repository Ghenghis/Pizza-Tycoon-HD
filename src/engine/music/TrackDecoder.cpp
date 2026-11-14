/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/TrackDecoder.h"

#include <algorithm>

#include "engine/SDL_include.h"
#include "engine/music/DrLibs.h"

namespace music {

namespace {

// Input frames handed to SDL's converter at a time.
constexpr std::size_t input_frames = 2048;

struct IoCloser {
  void operator()(SDL_IOStream* io) const noexcept { SDL_CloseIO(io); }
};
using IoPtr = std::unique_ptr<SDL_IOStream, IoCloser>;

// Opens file for reading through SDL (UTF-8 paths on every platform).
IoPtr open_io(const std::filesystem::path& file, std::uint64_t& bytes,
              std::string* error) {
  IoPtr io(SDL_IOFromFile(utf8_path(file).c_str(), "rb"));
  if (!io) {
    if (error) {
      *error =
          std::string("the file cannot be opened (") + SDL_GetError() + ")";
    }
    return {};
  }
  const Sint64 size = SDL_GetIOSize(io.get());
  if (size < 0) {
    if (error) {
      *error = "the file cannot be read";
    }
    return {};
  }
  bytes = static_cast<std::uint64_t>(size);
  return io;
}

SDL_IOStream* as_io(void* user) { return static_cast<SDL_IOStream*>(user); }

std::size_t io_read(void* user, void* out, std::size_t bytes) {
  return SDL_ReadIO(as_io(user), out, bytes);
}

bool io_seek(void* user, int offset, int origin) {
  const SDL_IOWhence whence = origin == 0   ? SDL_IO_SEEK_SET
                              : origin == 1 ? SDL_IO_SEEK_CUR
                                            : SDL_IO_SEEK_END;
  return SDL_SeekIO(as_io(user), offset, whence) >= 0;
}

bool io_tell(void* user, std::int64_t* cursor) {
  const Sint64 at = SDL_TellIO(as_io(user));
  if (at < 0) {
    return false;
  }
  *cursor = at;
  return true;
}

drwav_bool32 wav_seek(void* user, int offset, drwav_seek_origin origin) {
  const int o = origin == DRWAV_SEEK_SET ? 0 : origin == DRWAV_SEEK_CUR ? 1 : 2;
  return io_seek(user, offset, o) ? DRWAV_TRUE : DRWAV_FALSE;
}

drwav_bool32 wav_tell(void* user, drwav_int64* cursor) {
  std::int64_t at = 0;
  if (!io_tell(user, &at)) {
    return DRWAV_FALSE;
  }
  *cursor = at;
  return DRWAV_TRUE;
}

drmp3_bool32 mp3_seek(void* user, int offset, drmp3_seek_origin origin) {
  const int o = origin == DRMP3_SEEK_SET ? 0 : origin == DRMP3_SEEK_CUR ? 1 : 2;
  return io_seek(user, offset, o) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 mp3_tell(void* user, drmp3_int64* cursor) {
  std::int64_t at = 0;
  if (!io_tell(user, &at)) {
    return DRMP3_FALSE;
  }
  *cursor = at;
  return DRMP3_TRUE;
}

class WavDecoder final : public TrackDecoder {
public:
  WavDecoder(IoPtr io, StreamInfo info)
      : io_(std::move(io)), info_(std::move(info)),
        backend_(std::string("dr_wav ") + drwav_version_string()) {}
  ~WavDecoder() override {
    if (ready_) {
      drwav_uninit(&wav_);
    }
  }
  WavDecoder(const WavDecoder&) = delete;
  WavDecoder& operator=(const WavDecoder&) = delete;

  bool init(std::string* error) {
    if (SDL_SeekIO(io_.get(), 0, SDL_IO_SEEK_SET) < 0 ||
        !drwav_init(&wav_, io_read, wav_seek, wav_tell, io_.get(), nullptr)) {
      if (error) {
        *error = "the file is damaged: the WAV decoder cannot read it";
      }
      return false;
    }
    ready_ = true;
    if (wav_.channels != info_.channels ||
        wav_.sampleRate != info_.sample_rate) {
      if (error) {
        *error = "the file is damaged: its WAV header does not add up";
      }
      return false;
    }
    info_.frames = wav_.totalPCMFrameCount;
    return true;
  }

  [[nodiscard]] const StreamInfo& info() const override { return info_; }

  std::size_t read(std::span<float> out) override {
    const std::size_t want = out.size() / info_.channels;
    const auto got = static_cast<std::size_t>(
        drwav_read_pcm_frames_f32(&wav_, want, out.data()));
    if (got < want) {
      end_ = true;
      damaged_ = wav_.readCursorInPCMFrames < wav_.totalPCMFrameCount;
    }
    return got;
  }

  bool seek(std::uint64_t frame) override {
    if (!drwav_seek_to_pcm_frame(
            &wav_, std::min<std::uint64_t>(frame, info_.frames))) {
      return false;
    }
    end_ = false;
    return true;
  }

  [[nodiscard]] bool at_end() const override { return end_; }
  [[nodiscard]] bool damaged() const override { return damaged_; }
  [[nodiscard]] std::string_view backend() const override { return backend_; }

private:
  IoPtr io_;
  StreamInfo info_;
  std::string backend_;
  drwav wav_{};
  bool ready_ = false;
  bool end_ = false;
  bool damaged_ = false;
};

class Mp3Decoder final : public TrackDecoder {
public:
  Mp3Decoder(IoPtr io, StreamInfo info, const Limits& limits)
      : io_(std::move(io)), info_(std::move(info)), limits_(limits),
        backend_(std::string("dr_mp3 ") + drmp3_version_string()) {}
  ~Mp3Decoder() override {
    if (ready_) {
      drmp3_uninit(&mp3_);
    }
  }
  Mp3Decoder(const Mp3Decoder&) = delete;
  Mp3Decoder& operator=(const Mp3Decoder&) = delete;

  bool init(std::string* error) {
    // No metadata callback: dr_mp3 then skips the ID3v2 tag unread. Tags come
    // from read_stream_info().
    if (SDL_SeekIO(io_.get(), 0, SDL_IO_SEEK_SET) < 0 ||
        !drmp3_init(&mp3_, io_read, mp3_seek, mp3_tell, nullptr, io_.get(),
                    nullptr)) {
      if (error) {
        *error = "the file is damaged: the MP3 decoder cannot read it";
      }
      return false;
    }
    ready_ = true;
    if (mp3_.channels == 0 || mp3_.channels > limits_.max_channels ||
        mp3_.sampleRate < limits_.min_rate ||
        mp3_.sampleRate > limits_.max_rate) {
      if (error) {
        *error = "the file is damaged: its MP3 frames do not add up";
      }
      return false;
    }
    if (mp3_.sampleRate != info_.sample_rate && info_.sample_rate > 0) {
      info_.frames = info_.frames * mp3_.sampleRate / info_.sample_rate;
    }
    info_.channels = mp3_.channels;
    info_.sample_rate = mp3_.sampleRate;
    if (mp3_.totalPCMFrameCount != DRMP3_UINT64_MAX) {
      // What dr_mp3 plays: the encoder's delay and padding are left out.
      const std::uint64_t trimmed =
          std::uint64_t{mp3_.delayInPCMFrames} + mp3_.paddingInPCMFrames;
      info_.frames = mp3_.totalPCMFrameCount > trimmed
                         ? mp3_.totalPCMFrameCount - trimmed
                         : 0;
      info_.frames_exact = true;
    }
    return true;
  }

  [[nodiscard]] const StreamInfo& info() const override { return info_; }

  std::size_t read(std::span<float> out) override {
    const std::size_t want = out.size() / info_.channels;
    const auto got = static_cast<std::size_t>(
        drmp3_read_pcm_frames_f32(&mp3_, want, out.data()));
    decoded_ += got;
    if (got < want) {
      end_ = true;
      // An exact length that came up more than a few frames short.
      damaged_ = info_.frames_exact && decoded_ + 2 * 1152 < info_.frames;
    }
    return got;
  }

  bool seek(std::uint64_t frame) override {
    if (seek_points_.empty()) {
      // A table once, so later seeks do not decode from the start. It scans
      // the frame headers of the whole file, on the worker.
      drmp3_uint32 count = 512;
      seek_points_.resize(count);
      if (drmp3_calculate_seek_points(&mp3_, &count, seek_points_.data()) &&
          count > 0) {
        seek_points_.resize(count);
        drmp3_bind_seek_table(&mp3_, count, seek_points_.data());
      }
      else {
        seek_points_.assign(1, drmp3_seek_point{});
      }
    }
    if (!drmp3_seek_to_pcm_frame(&mp3_, frame)) {
      return false;
    }
    decoded_ = frame;
    end_ = false;
    return true;
  }

  [[nodiscard]] bool at_end() const override { return end_; }
  [[nodiscard]] bool damaged() const override { return damaged_; }
  [[nodiscard]] std::string_view backend() const override { return backend_; }

private:
  IoPtr io_;
  StreamInfo info_;
  Limits limits_;
  std::string backend_;
  drmp3 mp3_{};
  std::vector<drmp3_seek_point> seek_points_;
  std::uint64_t decoded_ = 0;
  bool ready_ = false;
  bool end_ = false;
  bool damaged_ = false;
};

} // namespace

std::optional<StreamInfo> probe_file(const std::filesystem::path& file,
                                     const Limits& limits, std::string* error) {
  std::uint64_t bytes = 0;
  IoPtr io = open_io(file, bytes, error);
  if (!io) {
    return std::nullopt;
  }
  return read_stream_info(io.get(), bytes, limits, error);
}

std::unique_ptr<TrackDecoder>
open_builtin_decoder(const std::filesystem::path& file, const Limits& limits,
                     std::string* error) {
  std::uint64_t bytes = 0;
  IoPtr io = open_io(file, bytes, error);
  if (!io) {
    return nullptr;
  }
  // The limits first: no decoder sees a file that breaks one.
  std::optional<StreamInfo> info =
      read_stream_info(io.get(), bytes, limits, error);
  if (!info) {
    return nullptr;
  }
  if (info->format == Format::WAV) {
    auto wav = std::make_unique<WavDecoder>(std::move(io), std::move(*info));
    return wav->init(error) ? std::move(wav) : nullptr;
  }
  if (info->format == Format::MP3) {
    auto mp3 =
        std::make_unique<Mp3Decoder>(std::move(io), std::move(*info), limits);
    return mp3->init(error) ? std::move(mp3) : nullptr;
  }
  if (error) {
    *error = "this format is not supported";
  }
  return nullptr;
}

/* TrackReader */

TrackReader::~TrackReader() { close(); }

bool TrackReader::open(std::unique_ptr<TrackDecoder> decoder,
                       std::string* error) {
  close();
  const StreamInfo& in = decoder->info();
  const SDL_AudioSpec src{
      .format = SDL_AUDIO_F32,
      .channels = static_cast<int>(in.channels),
      .freq = static_cast<int>(in.sample_rate),
  };
  const SDL_AudioSpec dst{
      .format = SDL_AUDIO_S16,
      .channels = 2,
      .freq = OUTPUT_RATE,
  };
  stream_ = SDL_CreateAudioStream(&src, &dst);
  if (!stream_) {
    if (error) {
      *error = std::string("its audio format cannot be converted (") +
               SDL_GetError() + ")";
    }
    return false;
  }
  input_.assign(input_frames * in.channels, 0.0f);
  decoder_ = std::move(decoder);
  position_ = 0;
  flushed_ = false;
  finished_ = false;
  failed_ = false;
  return true;
}

void TrackReader::close() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  decoder_.reset();
  finished_ = false;
}

std::size_t TrackReader::read(std::span<std::int16_t> out) {
  if (!decoder_) {
    return 0;
  }
  constexpr int frame_bytes = 2 * static_cast<int>(sizeof(std::int16_t));
  const std::size_t want = out.size() / 2;
  std::size_t got = 0;
  while (got < want && !finished_) {
    const int ready = SDL_GetAudioStreamAvailable(stream_);
    if (ready >= frame_bytes) {
      const int len =
          std::min(ready, static_cast<int>(want - got) * frame_bytes);
      const int n = SDL_GetAudioStreamData(stream_, out.data() + got * 2, len);
      if (n <= 0) {
        failed_ = true;
        finished_ = true;
        break;
      }
      got += static_cast<std::size_t>(n / frame_bytes);
      continue;
    }
    if (flushed_) {
      finished_ = true;
      break;
    }
    const std::size_t frames = decoder_->read(input_);
    if (frames == 0) {
      if (!decoder_->at_end()) {
        break; // nothing for now
      }
      SDL_FlushAudioStream(stream_);
      flushed_ = true;
      continue;
    }
    const int bytes =
        static_cast<int>(frames * decoder_->info().channels * sizeof(float));
    if (!SDL_PutAudioStreamData(stream_, input_.data(), bytes)) {
      failed_ = true;
      SDL_FlushAudioStream(stream_);
      flushed_ = true;
    }
  }
  position_ += got;
  return got;
}

bool TrackReader::seek(std::uint64_t output_frame) {
  if (!decoder_) {
    return false;
  }
  const std::uint64_t frame =
      output_frame * decoder_->info().sample_rate / OUTPUT_RATE;
  if (!decoder_->seek(frame)) {
    return false;
  }
  SDL_ClearAudioStream(stream_);
  position_ = output_frame;
  flushed_ = false;
  finished_ = false;
  return true;
}

bool TrackReader::damaged() const {
  return failed_ || (decoder_ && decoder_->damaged());
}

std::uint64_t TrackReader::duration() const {
  if (!decoder_ || decoder_->info().sample_rate == 0) {
    return 0;
  }
  const StreamInfo& in = decoder_->info();
  return in.frames * OUTPUT_RATE / in.sample_rate;
}

const StreamInfo& TrackReader::info() const {
  static const StreamInfo none;
  return decoder_ ? decoder_->info() : none;
}

std::string_view TrackReader::backend() const {
  return decoder_ ? decoder_->backend() : std::string_view{};
}

} // namespace music
