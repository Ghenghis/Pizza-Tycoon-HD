/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "engine/FormatCompat.h"

enum class LogLevel { Debug = 0, Info = 1, Warning = 2 };

// Thread-safe: any thread may log. Sinks are called one message at a time,
// under the logger's lock, so a sink must not block for long and must not
// add or remove sinks itself (logging from a sink is allowed).
class Logger {
public:
  using SinkFunc = std::function<void(LogLevel, const std::string&)>;
  using SinkId = std::size_t;
  // A sink gets the messages at or above its own level when it has one,
  // else at or above the global level. The id removes it again.
  static SinkId add_sink(SinkFunc, std::optional<LogLevel> min = std::nullopt);
  static void remove_sink(SinkId);
  static void clear_sinks();

  // Removes the sink when it goes out of scope (sinks that capture locals).
  class ScopedSink {
  public:
    explicit ScopedSink(SinkId id) : id_(id) {}
    ~ScopedSink() { Logger::remove_sink(id_); }
    ScopedSink(const ScopedSink&) = delete;
    ScopedSink& operator=(const ScopedSink&) = delete;

  private:
    SinkId id_;
  };

  // Writes the log to <folder>/plegacy-<date>-<time>.log (Info and up),
  // keeping the newest `keep` such files. Returns the file's path, or empty
  // when it can't be written (the game runs on without a log file).
  static std::string start_log_file(const std::string& folder, int keep = 5);

  static void global_min_level(LogLevel);
  static LogLevel global_min_level();

  static void log(LogLevel, const std::string& msg);

  template <typename... Args>
  static void debug(compat::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Debug, compat::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void info(compat::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Info, compat::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args>
  static void warn(compat::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Warning, compat::format(fmt, std::forward<Args>(args)...));
  }

  static std::string level_prefix(LogLevel);

private:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct Sink {
    SinkId id;
    SinkFunc func;
    std::optional<LogLevel> min;
  };
  static std::vector<Sink>& sinks_();
  static LogLevel& min_lvl_();
  static TimePoint& start_time_();
  static TimePoint& last_time_();
};
