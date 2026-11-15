/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Recording (R34, docs/studio/notes/recording.md): the owned child process,
// and a real session through the real FFmpeg (skipped when there is none),
// checked by decoding what it wrote with ffprobe.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "engine/audio/MixTap.h"
#include "engine/record/RecordingSession.h"
#include "engine/record/Subprocess.h"

using record::Subprocess;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

TEST_CASE("a Windows command line gives back exactly its arguments",
          "[record]") {
  CHECK(Subprocess::windows_command_line({"ffmpeg", "-i", "pipe:0"}) ==
        "ffmpeg -i pipe:0");
  CHECK(Subprocess::windows_command_line({"a b", ""}) == "\"a b\" \"\"");
  CHECK(Subprocess::windows_command_line({R"(say "hi")"}) == R"("say \"hi\"")");
  // Backslashes double only before a quote, the closing one included.
  CHECK(Subprocess::windows_command_line({R"(C:\Games\Pizza Tycoon\)"}) ==
        R"("C:\Games\Pizza Tycoon\\")");
  CHECK(Subprocess::windows_command_line({R"(C:\no\space)"}) ==
        R"(C:\no\space)");
  CHECK(Subprocess::windows_command_line({R"(a\"b c)"}) == R"("a\\\"b c")");
}

#if !defined(_WIN32)
TEST_CASE("the child gets both inputs, its errors are kept, and it cannot "
          "outlive its owner",
          "[record]") {
  const auto sh = record::find_program("sh");
  REQUIRE(sh);
  const std::string hello = "hello from the first pipe\n";
  const auto bytes = [](const std::string& s) {
    return std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  };

  SECTION("both inputs") {
    std::string why;
    // Arguments go as given: the quoting is sh's own, inside one argument.
    auto p = Subprocess::start(
        *sh,
        {"-c", "cat >&2; cat <&3 >&2", std::string(Subprocess::SECOND_INPUT)},
        &why);
    REQUIRE(p);
    CHECK(p->write(Subprocess::Input::FIRST, bytes(hello)));
    p->close(Subprocess::Input::FIRST);
    CHECK(p->write(Subprocess::Input::SECOND, bytes("and the second\n")));
    p->close(Subprocess::Input::SECOND);
    REQUIRE(p->wait(10s) == 0);
    CHECK(p->stderr_tail() == hello + "and the second\n");
  }
  SECTION("a child that has gone makes writes fail, not the game") {
    auto p = Subprocess::start(*sh, {"-c", "exit 3"}, nullptr);
    REQUIRE(p);
    REQUIRE(p->wait(10s) == 3);
    std::vector<std::uint8_t> big(1 << 20, 'x');
    CHECK_FALSE(p->write(Subprocess::Input::FIRST, big));
  }
  SECTION("destroying it ends the child") {
    auto p = Subprocess::start(*sh, {"-c", "sleep 30"}, nullptr);
    REQUIRE(p);
    const auto t0 = std::chrono::steady_clock::now();
    p.reset();
    CHECK(std::chrono::steady_clock::now() - t0 < 5s);
  }
  SECTION("run() keeps the output and the exit code") {
    const auto out =
        Subprocess::run(*sh, {"-c", "echo out; echo err >&2; exit 2"}, 10s);
    REQUIRE(out);
    CHECK(out->exit_code == 2);
    CHECK(out->text.find("out") != std::string::npos);
    CHECK(out->text.find("err") != std::string::npos);
  }
  SECTION("a program that is not there ends at once, with 127") {
    auto p = Subprocess::start("/nonexistent/ffmpeg", {}, nullptr);
    REQUIRE(p);
    CHECK(p->wait(10s) == 127);
  }
}
#endif

TEST_CASE("three seconds of a game, through the real FFmpeg, decode as "
          "recorded",
          "[record][ffmpeg]") {
  const auto caps = record::Capabilities::probe();
  if (!caps.why_not.empty()) {
    SKIP(caps.why_not);
  }
  const auto ffprobe = record::find_program("ffprobe");
  if (!ffprobe) {
    SKIP("ffprobe is not on the PATH");
  }
  // Only encoders that opened a trial are offered; hardware ones are not.
  for (const std::string& e : caps.offered) {
    CHECK(e.find("nvenc") == std::string::npos);
    CHECK(e.find("amf") == std::string::npos);
  }

  test_support::TempDir dir;
  audio::MixTap tap;
  tap.enabled(true);
  tap.write(std::vector<std::int16_t>(4410 * 2, 30000)); // before: not kept
  record::Config config;
  config.folder = dir.path();
  config.segment_seconds = 1;
  config.reserve_bytes = 0;
  config.max_duration = 3s;
  std::string why;
  auto session = record::RecordingSession::start(config, caps, tap, &why);
  INFO(why);
  REQUIRE(session);

  // The device: a tone at its own pace. The game: 60 frames a second.
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<bool> run{true};
  std::thread device([&] {
    std::vector<std::int16_t> block(512 * 2);
    std::uint64_t made = 0;
    while (run) {
      const double el =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
      while (made < static_cast<std::uint64_t>(el * 44100)) {
        for (std::size_t f = 0; f < 512; ++f) {
          const double t = static_cast<double>(made + f) / 44100.0;
          block[f * 2] = block[f * 2 + 1] = static_cast<std::int16_t>(
              std::lround(3000 * std::sin(2 * 3.14159265358979 * 220 * t)));
        }
        tap.write(block);
        made += 512;
      }
      std::this_thread::sleep_for(5ms);
    }
  });
  std::vector<std::uint32_t> pixels(640 * 400, 0x406080FFu);
  while (!session->finished() && std::chrono::steady_clock::now() - t0 < 20s) {
    const auto now = std::chrono::steady_clock::now();
    if (session->wants_video(now)) {
      session->submit_video(pixels.data(), 640, 400, 640 * 4, now);
    }
    std::this_thread::sleep_for(16ms);
  }
  session->wait_finished(); // it stops by itself at 3 s
  run = false;
  device.join();

  const record::Status s = session->status();
  INFO(s.error);
  REQUIRE(s.state == record::State::STOPPED);
  CHECK(s.stop_reason == record::StopReason::DURATION);
  CHECK(s.frames_written == 90);
  CHECK(s.audio_samples == 90 * 1470); // exactly as long as the picture
  CHECK(s.frames_dropped == 0);
  REQUIRE(s.segments.size() >= 3);

  std::ifstream manifest_file(s.folder / "recording.json");
  const auto manifest = nlohmann::json::parse(manifest_file);
  CHECK(manifest["format"] == "pthd-recording");
  CHECK(manifest["complete"] == true);
  CHECK(manifest["video"]["frames"] == 90);

  // Decoded: every segment has both streams, and the frames add up.
  std::uint64_t frames = 0;
  for (const record::Segment& seg : s.segments) {
    const fs::path file = s.folder / seg.file;
    REQUIRE(fs::exists(file));
    const auto streams =
        Subprocess::run(*ffprobe,
                        {"-v", "error", "-show_entries", "stream=codec_type",
                         "-of", "csv=p=0", file.string()},
                        30s);
    REQUIRE(streams);
    CHECK(streams->text.find("video") != std::string::npos);
    CHECK(streams->text.find("audio") != std::string::npos);
    const auto counted =
        Subprocess::run(*ffprobe,
                        {"-v", "error", "-count_frames", "-select_streams",
                         "v:0", "-show_entries", "stream=nb_read_frames", "-of",
                         "csv=p=0", file.string()},
                        30s);
    REQUIRE(counted);
    frames += std::stoull(counted->text);
  }
  CHECK(frames == 90);
}
