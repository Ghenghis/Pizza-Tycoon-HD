/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/AudioHeaders.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "engine/SDL_include.h"

namespace music {

namespace {

// Chunks walked in a WAV file, and frames walked in an ID3v2 tag.
constexpr int max_chunks = 512;
constexpr int max_tag_frames = 512;
// The largest format chunk and tag text read.
constexpr std::uint64_t max_fmt_bytes = 1024;
constexpr std::uint64_t max_text_bytes = 4096;
// The largest LIST chunk read for its tags; bigger ones are skipped.
constexpr std::uint64_t max_list_bytes = 64 * 1024;
// How far past the tags the first MPEG frame may start.
constexpr std::size_t mp3_sync_window = 64 * 1024;

std::optional<StreamInfo> refuse(std::string* error, std::string why) {
  if (error) {
    *error = std::move(why);
  }
  return std::nullopt;
}

// Reads exactly out.size() bytes at offset.
bool read_at(SDL_IOStream* io, std::uint64_t offset,
             std::span<std::uint8_t> out) {
  if (SDL_SeekIO(io, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) {
    return false;
  }
  return SDL_ReadIO(io, out.data(), out.size()) == out.size();
}

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
  return std::uint32_t{b[at]} | (std::uint32_t{b[at + 1]} << 8) |
         (std::uint32_t{b[at + 2]} << 16) | (std::uint32_t{b[at + 3]} << 24);
}

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
  return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
         (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

std::uint32_t syncsafe32(std::span<const std::uint8_t> b, std::size_t at) {
  return (std::uint32_t{b[at] & 0x7Fu} << 21) |
         (std::uint32_t{b[at + 1] & 0x7Fu} << 14) |
         (std::uint32_t{b[at + 2] & 0x7Fu} << 7) |
         std::uint32_t{b[at + 3] & 0x7Fu};
}

std::string_view chars(std::span<const std::uint8_t> b, std::size_t at,
                       std::size_t n) {
  return {reinterpret_cast<const char*>(b.data() + at), n};
}

void set_tag(std::string& tag, std::string_view text) {
  if (tag.empty()) {
    tag = clean_text(text, MAX_TITLE_BYTES);
  }
}

// Checks what both formats share; empty when the stream is acceptable.
std::string check_limits(const StreamInfo& info, const Limits& limits) {
  if (info.channels == 0) {
    return "the file is damaged: it says it has no channels";
  }
  if (info.channels > limits.max_channels) {
    return "the file has " + std::to_string(info.channels) +
           " channels; at most " + std::to_string(limits.max_channels) +
           " are supported";
  }
  if (info.sample_rate < limits.min_rate ||
      info.sample_rate > limits.max_rate) {
    return "its sample rate of " + std::to_string(info.sample_rate) +
           " Hz is not supported (" + std::to_string(limits.min_rate) + " to " +
           std::to_string(limits.max_rate) + " Hz)";
  }
  if (info.frames == 0) {
    return "the file has no audio";
  }
  if (info.frames / info.sample_rate >= limits.max_seconds) {
    return limits.max_seconds >= 120
               ? "the track is longer than " +
                     std::to_string(limits.max_seconds / 60) + " minutes"
               : "the track is longer than " +
                     std::to_string(limits.max_seconds) + " seconds";
  }
  return {};
}

/* WAV */

// KSDATAFORMAT_SUBTYPE_*: the GUID of WAVE_FORMAT_EXTENSIBLE after its first
// two bytes, which hold the format tag.
constexpr std::array<std::uint8_t, 14> wave_guid_tail = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
    0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

void read_info_tags(SDL_IOStream* io, std::uint64_t at, std::uint64_t size,
                    StreamInfo& info) {
  if (size < 4 || size > max_list_bytes) {
    return;
  }
  std::vector<std::uint8_t> list(static_cast<std::size_t>(size));
  if (!read_at(io, at, list) || chars(list, 0, 4) != "INFO") {
    return;
  }
  std::size_t pos = 4;
  while (pos + 8 <= list.size()) {
    const std::string_view id = chars(list, pos, 4);
    const std::uint32_t n = le32(list, pos + 4);
    pos += 8;
    if (n > list.size() - pos) {
      break;
    }
    const std::string_view text = chars(list, pos, n);
    if (id == "INAM") {
      set_tag(info.title, text);
    }
    else if (id == "IART") {
      set_tag(info.artist, text);
    }
    else if (id == "IPRD") {
      set_tag(info.album, text);
    }
    pos += n + (n & 1);
  }
}

// Parses a format chunk into info; empty when it is one we play.
std::string parse_fmt(std::span<const std::uint8_t> fmt, StreamInfo& info,
                      std::uint16_t& block_align) {
  std::uint16_t tag = le16(fmt, 0);
  info.channels = le16(fmt, 2);
  info.sample_rate = le32(fmt, 4);
  block_align = le16(fmt, 12);
  const std::uint16_t bits = le16(fmt, 14);
  if (tag == 0xFFFE) {
    if (fmt.size() < 40 ||
        !std::equal(wave_guid_tail.begin(), wave_guid_tail.end(),
                    fmt.begin() + 26)) {
      return "this WAV encoding is not supported";
    }
    tag = le16(fmt, 24);
  }
  bool ok = false;
  switch (tag) {
  case 1:
    ok = bits == 8 || bits == 16 || bits == 24 || bits == 32;
    info.encoding = "PCM " + std::to_string(bits) + "-bit";
    break;
  case 3:
    ok = bits == 32 || bits == 64;
    info.encoding = "float " + std::to_string(bits) + "-bit";
    break;
  case 6:
  case 7:
    ok = bits == 8;
    info.encoding = tag == 6 ? "A-law" : "mu-law";
    break;
  default:
    break;
  }
  if (!ok) {
    return "this WAV encoding is not supported (format " + std::to_string(tag) +
           ", " + std::to_string(bits) + " bits)";
  }
  if (info.channels == 0 || block_align != info.channels * (bits / 8)) {
    return "the file is damaged: its WAV header does not add up";
  }
  info.bitrate_kbps = static_cast<std::uint32_t>(
      std::uint64_t{info.sample_rate} * block_align * 8 / 1000);
  return {};
}

std::optional<StreamInfo> read_wav(SDL_IOStream* io, std::uint64_t file_bytes,
                                   const Limits& limits, std::string* error) {
  std::array<std::uint8_t, 12> riff{};
  if (file_bytes < riff.size() || !read_at(io, 0, riff)) {
    return refuse(error, "the file is too short to be a WAV file");
  }
  const std::string_view kind = chars(riff, 0, 4);
  if (kind != "RIFF") {
    return refuse(error, "this kind of WAV file is not supported (" +
                             clean_text(kind, 4) + ")");
  }
  if (chars(riff, 8, 4) != "WAVE") {
    return refuse(error, "not a WAV file");
  }

  StreamInfo info;
  info.format = Format::WAV;
  info.frames_exact = true;
  std::uint16_t block_align = 0;
  bool have_fmt = false;
  bool have_data = false;
  std::uint64_t at = 12;
  int chunks = 0;
  for (; at + 8 <= file_bytes && chunks < max_chunks; ++chunks) {
    std::array<std::uint8_t, 8> head{};
    if (!read_at(io, at, head)) {
      break;
    }
    const std::string_view id = chars(head, 0, 4);
    const std::uint64_t size = le32(head, 4);
    const std::uint64_t body = at + 8;
    const std::uint64_t left = file_bytes - body;

    if (have_data) {
      // After the audio only tags are read, and nothing there can make the
      // file fail.
      if (size > left) {
        break;
      }
      if (id == "LIST") {
        read_info_tags(io, body, size, info);
      }
    }
    else if (id == "data") {
      if (!have_fmt) {
        return refuse(error,
                      "the file is damaged: its audio comes before its format");
      }
      if (body > limits.max_header_bytes) {
        return refuse(error, "the WAV header is too large (over " +
                                 mib(limits.max_header_bytes) +
                                 " before the audio)");
      }
      if (size > left) {
        return refuse(error, "the file is cut short: its header promises " +
                                 std::to_string(size) + " bytes of audio, " +
                                 std::to_string(left) + " are there");
      }
      info.frames = block_align > 0 ? size / block_align : 0;
      have_data = true;
    }
    else {
      if (id == "fmt " && (size < 16 || size > max_fmt_bytes)) {
        return refuse(error, "the file is damaged: its format chunk has an "
                             "impossible size (" +
                                 std::to_string(size) + " bytes)");
      }
      if (size > left) {
        return refuse(
            error, "the file is cut short: a header chunk runs past its end");
      }
      if (body + size > limits.max_header_bytes) {
        return refuse(error, "the WAV header is too large (over " +
                                 mib(limits.max_header_bytes) +
                                 " before the audio)");
      }
      if (id == "fmt ") {
        if (have_fmt) {
          return refuse(error, "the file is damaged: it has two formats");
        }
        std::vector<std::uint8_t> fmt(static_cast<std::size_t>(size));
        if (!read_at(io, body, fmt)) {
          return refuse(error, "the file could not be read");
        }
        if (std::string why = parse_fmt(fmt, info, block_align); !why.empty()) {
          return refuse(error, std::move(why));
        }
        have_fmt = true;
      }
      else if (id == "LIST") {
        read_info_tags(io, body, size, info);
      }
    }
    at = body + size + (size & 1);
  }
  if (!have_data) {
    if (chunks >= max_chunks) {
      return refuse(error, "the WAV header has too many chunks");
    }
    return refuse(error, have_fmt ? "the file has no audio"
                                  : "the file is damaged: it has no format");
  }
  if (std::string why = check_limits(info, limits); !why.empty()) {
    return refuse(error, std::move(why));
  }
  return info;
}

/* MP3 */

struct MpegHeader {
  int version = 0; // 1, 2 or 25 (MPEG-2.5)
  int layer = 0;   // 1, 2 or 3
  std::uint32_t bitrate_kbps = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t samples = 0; // per frame
  std::uint32_t bytes = 0;   // frame length
};

constexpr std::array<std::array<std::uint16_t, 15>, 5> bitrates = {{
    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
}};

std::optional<MpegHeader> parse_mpeg(std::span<const std::uint8_t> h) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
    return std::nullopt;
  }
  const int v = (h[1] >> 3) & 3;
  const int l = (h[1] >> 1) & 3;
  const int b = h[2] >> 4;
  const int r = (h[2] >> 2) & 3;
  if (v == 1 || l == 0 || b == 0 || b == 15 || r == 3) {
    return std::nullopt; // reserved, or free format
  }
  MpegHeader m;
  m.version = v == 3 ? 1 : v == 2 ? 2 : 25;
  m.layer = 4 - l;
  const int table = m.version == 1 ? m.layer - 1 : (m.layer == 1 ? 3 : 4);
  m.bitrate_kbps =
      bitrates[static_cast<std::size_t>(table)][static_cast<std::size_t>(b)];
  constexpr std::array<std::uint32_t, 3> rates = {44100, 48000, 32000};
  m.sample_rate = rates[static_cast<std::size_t>(r)] / (m.version == 1   ? 1
                                                        : m.version == 2 ? 2
                                                                         : 4);
  m.channels = (h[3] >> 6) == 3 ? 1 : 2;
  m.samples =
      m.layer == 1 ? 384 : (m.layer == 3 && m.version != 1 ? 576 : 1152);
  const std::uint32_t padding = (h[2] >> 1) & 1;
  if (m.layer == 1) {
    m.bytes = (12 * m.bitrate_kbps * 1000 / m.sample_rate + padding) * 4;
  }
  else {
    m.bytes = m.samples / 8 * m.bitrate_kbps * 1000 / m.sample_rate + padding;
  }
  return m;
}

// Text of an ID3v2 text frame: an encoding byte, then the text.
std::string id3_text(std::span<const std::uint8_t> frame) {
  if (frame.empty()) {
    return {};
  }
  const std::uint8_t encoding = frame[0];
  const auto text = frame.subspan(1);
  if (encoding == 0) {
    std::string latin1(reinterpret_cast<const char*>(text.data()), text.size());
    std::string out;
    for (const char c : latin1.substr(0, latin1.find('\0'))) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x80) {
        out.push_back(c);
      }
      else {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
      }
    }
    return out;
  }
  if (encoding == 3) {
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
  }
  if (encoding != 1 && encoding != 2) {
    return {};
  }
  // UTF-16, with a byte order mark (1) or big-endian (2).
  bool big = encoding == 2;
  std::size_t i = 0;
  if (encoding == 1 && text.size() >= 2) {
    if (text[0] == 0xFF && text[1] == 0xFE) {
      i = 2;
    }
    else if (text[0] == 0xFE && text[1] == 0xFF) {
      big = true;
      i = 2;
    }
  }
  std::string out;
  const auto unit = [&](std::size_t at) -> std::uint32_t {
    return big ? (std::uint32_t{text[at]} << 8) | text[at + 1]
               : (std::uint32_t{text[at + 1]} << 8) | text[at];
  };
  while (i + 1 < text.size()) {
    std::uint32_t cp = unit(i);
    i += 2;
    if (cp == 0) {
      break;
    }
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size()) {
      const std::uint32_t low = unit(i);
      if (low >= 0xDC00 && low < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        i += 2;
      }
    }
    if (cp >= 0xD800 && cp < 0xE000) {
      continue; // a lone surrogate
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// Reads the title, artist and album from an ID3v2 tag of tag_bytes at 10.
void read_id3v2(SDL_IOStream* io, std::span<const std::uint8_t> head,
                std::uint64_t tag_bytes, StreamInfo& info) {
  const int version = head[3];
  const std::uint8_t flags = head[5];
  if (version < 2 || version > 4 || (flags & 0x80) != 0) {
    return; // unknown, or unsynchronised throughout
  }
  std::uint64_t at = 10;
  const std::uint64_t end = 10 + tag_bytes;
  if ((flags & 0x40) != 0 && version >= 3) {
    std::array<std::uint8_t, 4> ext{};
    if (!read_at(io, at, ext)) {
      return;
    }
    at += version == 4 ? syncsafe32(ext, 0) : be32(ext, 0) + 4u;
  }
  const std::size_t id_bytes = version == 2 ? 3 : 4;
  const std::size_t head_bytes = version == 2 ? 6 : 10;
  for (int n = 0; n < max_tag_frames && at + head_bytes <= end; ++n) {
    std::array<std::uint8_t, 10> fh{};
    if (!read_at(io, at, std::span(fh).first(head_bytes))) {
      return;
    }
    if (fh[0] == 0) {
      return; // padding
    }
    std::uint64_t size = 0;
    std::uint16_t frame_flags = 0;
    if (version == 2) {
      size = (std::uint64_t{fh[3]} << 16) | (std::uint64_t{fh[4]} << 8) | fh[5];
    }
    else {
      size = version == 4 ? syncsafe32(fh, 4) : be32(fh, 4);
      frame_flags = static_cast<std::uint16_t>((fh[8] << 8) | fh[9]);
    }
    const std::uint64_t body = at + head_bytes;
    if (size > end - body) {
      return;
    }
    // Compressed, encrypted or unsynchronised frames are skipped.
    const bool plain = version == 4   ? (frame_flags & 0x000F) == 0
                       : version == 3 ? (frame_flags & 0x00C0) == 0
                                      : true;
    const std::string_view id = chars(fh, 0, id_bytes);
    std::string* tag = nullptr;
    if (id == "TIT2" || id == "TT2") {
      tag = &info.title;
    }
    else if (id == "TPE1" || id == "TP1") {
      tag = &info.artist;
    }
    else if (id == "TALB" || id == "TAL") {
      tag = &info.album;
    }
    if (tag && plain && tag->empty() && size > 0 && size <= max_text_bytes) {
      std::vector<std::uint8_t> frame(static_cast<std::size_t>(size));
      if (read_at(io, body, frame)) {
        set_tag(*tag, id3_text(frame));
      }
    }
    at = body + size;
  }
}

std::optional<StreamInfo> read_mp3(SDL_IOStream* io, std::uint64_t file_bytes,
                                   const Limits& limits, std::string* error) {
  StreamInfo info;
  info.format = Format::MP3;
  std::uint64_t audio = 0;
  std::array<std::uint8_t, 10> head{};
  if (file_bytes >= head.size() && read_at(io, 0, head) &&
      chars(head, 0, 3) == "ID3") {
    std::uint64_t tag_bytes = syncsafe32(head, 6);
    audio = 10 + tag_bytes + ((head[5] & 0x10) != 0 ? 10 : 0);
    if (audio > limits.max_header_bytes) {
      return refuse(error, "its tags are too large (over " +
                               mib(limits.max_header_bytes) + ")");
    }
    if (audio >= file_bytes) {
      return refuse(error, "the file is cut short: it holds tags but no audio");
    }
    read_id3v2(io, head, tag_bytes, info);
  }

  // The first MPEG frame, close after the tags.
  const std::size_t window = static_cast<std::size_t>(
      std::min<std::uint64_t>(mp3_sync_window, file_bytes - audio));
  std::vector<std::uint8_t> data(window);
  if (!read_at(io, audio, data)) {
    return refuse(error, "the file could not be read");
  }
  std::optional<MpegHeader> first;
  std::size_t found = 0;
  for (std::size_t i = 0; i + 4 <= data.size() && !first; ++i) {
    if (auto m = parse_mpeg(std::span(data).subspan(i, 4))) {
      // A real frame is followed by another, or by the end of the file.
      const std::size_t next = i + m->bytes;
      if (next + 4 <= data.size()) {
        const auto again = parse_mpeg(std::span(data).subspan(next, 4));
        if (!again || again->sample_rate != m->sample_rate) {
          continue;
        }
      }
      first = m;
      found = i;
    }
  }
  if (!first) {
    return refuse(error, "not an MP3 file (no audio frames found)");
  }
  info.sample_rate = first->sample_rate;
  info.channels = first->channels;
  info.bitrate_kbps = first->bitrate_kbps;
  info.encoding = std::string(first->version == 1   ? "MPEG-1"
                              : first->version == 2 ? "MPEG-2"
                                                    : "MPEG-2.5") +
                  " Layer " +
                  (first->layer == 1   ? "I"
                   : first->layer == 2 ? "II"
                                       : "III");

  bool tail_tag = false;
  if (file_bytes >= 128 + audio) {
    std::array<std::uint8_t, 128> v1{};
    if (read_at(io, file_bytes - 128, v1) && chars(v1, 0, 3) == "TAG") {
      tail_tag = true;
      set_tag(info.title, chars(v1, 3, 30));
      set_tag(info.artist, chars(v1, 33, 30));
      set_tag(info.album, chars(v1, 63, 30));
    }
  }

  // The length: from a Xing, Info or VBRI frame when there is one, otherwise
  // from the size at the first frame's bitrate.
  const std::size_t side = first->version == 1
                               ? (first->channels == 1 ? 17 : 32)
                               : (first->channels == 1 ? 9 : 17);
  const std::size_t xing = found + 4 + side;
  const std::size_t vbri = found + 4 + 32;
  if (xing + 12 <= data.size() &&
      (chars(data, xing, 4) == "Xing" || chars(data, xing, 4) == "Info")) {
    if ((be32(data, xing + 4) & 1) != 0) {
      info.frames = std::uint64_t{be32(data, xing + 8)} * first->samples;
      info.frames_exact = true;
    }
  }
  else if (vbri + 18 <= data.size() && chars(data, vbri, 4) == "VBRI") {
    info.frames = std::uint64_t{be32(data, vbri + 14)} * first->samples;
    info.frames_exact = true;
  }
  if (!info.frames_exact) {
    const std::uint64_t bytes =
        file_bytes - audio - found - (tail_tag ? 128 : 0);
    info.frames = bytes * 8 * info.sample_rate /
                  (std::uint64_t{first->bitrate_kbps} * 1000);
  }
  if (std::string why = check_limits(info, limits); !why.empty()) {
    return refuse(error, std::move(why));
  }
  return info;
}

} // namespace

std::optional<Format> sniff_format(std::span<const std::uint8_t> head) {
  if (head.size() < 12) {
    return std::nullopt;
  }
  const std::string_view id = chars(head, 0, 4);
  if (id == "RIFF" || id == "RIFX" || id == "RF64") {
    return Format::WAV;
  }
  if (id == "fLaC") {
    return Format::FLAC;
  }
  if (id == "OggS") {
    return Format::OGG_VORBIS;
  }
  if (chars(head, 0, 3) == "ID3" || parse_mpeg(head.first(4))) {
    return Format::MP3;
  }
  return std::nullopt;
}

std::optional<StreamInfo> read_stream_info(SDL_IOStream* io,
                                           std::uint64_t file_bytes,
                                           const Limits& limits,
                                           std::string* error) {
  if (file_bytes == 0) {
    return refuse(error, "the file is empty");
  }
  if (file_bytes > limits.max_file_bytes) {
    return refuse(error,
                  "the file is larger than " + mib(limits.max_file_bytes));
  }
  std::array<std::uint8_t, 12> head{};
  if (file_bytes < head.size() || !read_at(io, 0, head)) {
    return refuse(error, "the file is too short to hold audio");
  }
  const std::optional<Format> format = sniff_format(head);
  if (!format) {
    return refuse(error, "not a supported audio file (MP3 or WAV)");
  }
  switch (*format) {
  case Format::WAV:
    return read_wav(io, file_bytes, limits, error);
  case Format::MP3:
    return read_mp3(io, file_bytes, limits, error);
  case Format::FLAC:
    return refuse(error, "FLAC files are not supported");
  case Format::OGG_VORBIS:
    return refuse(error, "Ogg files are not supported");
  }
  return refuse(error, "not a supported audio file (MP3 or WAV)");
}

} // namespace music
