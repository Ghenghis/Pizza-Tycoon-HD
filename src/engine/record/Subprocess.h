/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// A child process the recorder owns (docs/studio/notes/recording.md,
// "Encoder process"): FFmpeg, started without a shell and without a window,
// with two input pipes and its error output read on its own thread. It
// cannot outlive the game: on Windows it is in a Job object that kills it
// when the game's handle closes, on Linux it gets SIGKILL when the thread
// that started it ends (start it from a thread that lives as long as it
// should). On POSIX it leads its own process group, and a kill takes the
// whole group.
namespace record {

class Subprocess {
public:
  // Where the second input pipe is, as the child is told: "pipe:3" (POSIX)
  // or a named pipe path (Windows). Pass it in args as {second_input}.
  static constexpr std::string_view SECOND_INPUT = "{second_input}";

  // Starts exe with args (no shell: each is one argument as given; an
  // argument equal to SECOND_INPUT is replaced by where that pipe is). stdin is
  // the first pipe, the second pipe is the second input, stdout goes nowhere
  // and stderr is kept (its last stderr_tail bytes). Null, with the reason,
  // when it cannot.
  static std::unique_ptr<Subprocess> start(const std::filesystem::path& exe,
                                           const std::vector<std::string>& args,
                                           std::string* why,
                                           std::size_t stderr_tail = 64 * 1024);
  ~Subprocess(); // kills it if it still runs, and waits for it
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;

  enum class Input { FIRST, SECOND };
  // Blocks until every byte is written. False when the child no longer
  // reads (it ended or closed the pipe). Each input has one writer thread.
  bool write(Input input, std::span<const std::uint8_t> bytes);
  void close(Input input);

  // The exit code once the child has ended, within timeout; nullopt when it
  // still runs. A child killed by a signal reports 128 + the signal.
  std::optional<int> wait(std::chrono::milliseconds timeout);
  void kill();
  [[nodiscard]] std::string stderr_tail() const;

  // How args reach a Windows child: one command line, quoted so that
  // CommandLineToArgvW gives back exactly these arguments.
  [[nodiscard]] static std::string
  windows_command_line(const std::vector<std::string>& argv);

  // Runs exe with args to the end, with no input, and returns its exit code
  // and output (stdout and stderr, at most max_output bytes); nullopt when
  // it cannot start or takes longer than timeout (it is killed then).
  struct Output {
    int exit_code = -1;
    std::string text;
  };
  static std::optional<Output> run(const std::filesystem::path& exe,
                                   const std::vector<std::string>& args,
                                   std::chrono::milliseconds timeout,
                                   std::size_t max_output = 1 << 20);

private:
  Subprocess() = default;
  // merge_stdout: stdout goes where stderr does (run()).
  static std::unique_ptr<Subprocess>
  start_impl(const std::filesystem::path& exe,
             const std::vector<std::string>& args, std::string* why,
             std::size_t stderr_tail, bool merge_stdout);
  void read_stderr();

  struct Native;
  std::unique_ptr<Native> native_;
  std::thread stderr_thread_;
  mutable std::mutex tail_mutex_;
  std::string tail_;
  std::size_t tail_limit_ = 0;
  std::optional<int> exit_code_;
};

// The executable to run for name ("ffmpeg"): explicit when given and
// there, else the first match on the PATH (with ".exe" on Windows).
[[nodiscard]] std::optional<std::filesystem::path>
find_program(const std::string& name,
             const std::optional<std::filesystem::path>& explicit_path = {});

} // namespace record
