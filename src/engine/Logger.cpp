/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/Logger.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include "engine/FormatCompat.h"

namespace {
// Recursive: a sink may log (a warning about itself, say).
std::recursive_mutex& log_mutex() {
  static std::recursive_mutex m;
  return m;
}
Logger::SinkId next_sink_id = 1;
} // namespace

std::vector<Logger::Sink>& Logger::sinks_() {
  static std::vector<Sink> v;
  return v;
}

LogLevel& Logger::min_lvl_() {
#ifdef NDEBUG
  static LogLevel lvl = LogLevel::Warning;
#else
  static LogLevel lvl = LogLevel::Debug;
#endif
  return lvl;
}

Logger::TimePoint& Logger::start_time_() {
  static TimePoint t = Clock::now();
  return t;
}

Logger::TimePoint& Logger::last_time_() {
  static TimePoint t = Clock::now();
  return t;
}

Logger::SinkId Logger::add_sink(SinkFunc sink, std::optional<LogLevel> min) {
  std::lock_guard lock(log_mutex());
  const SinkId id = next_sink_id++;
  sinks_().push_back({id, std::move(sink), min});
  return id;
}

void Logger::remove_sink(SinkId id) {
  std::lock_guard lock(log_mutex());
  std::erase_if(sinks_(), [id](const Sink& s) { return s.id == id; });
}

void Logger::clear_sinks() {
  std::lock_guard lock(log_mutex());
  sinks_().clear();
}

void Logger::global_min_level(LogLevel lvl) {
  std::lock_guard lock(log_mutex());
  min_lvl_() = lvl;
}

LogLevel Logger::global_min_level() {
  std::lock_guard lock(log_mutex());
  return min_lvl_();
}

std::string Logger::start_log_file(const std::string& folder, int keep) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(folder, ec);
  // Oldest first by name (the names sort by time): keep the newest keep-1
  // plus the one started now.
  std::vector<fs::path> old;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file(ec) && name.starts_with("plegacy-") &&
        name.ends_with(".log")) {
      old.push_back(entry.path());
    }
  }
  std::sort(old.begin(), old.end());
  while (!old.empty() && static_cast<int>(old.size()) > std::max(0, keep - 1)) {
    fs::remove(old.front(), ec);
    old.erase(old.begin());
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
  const fs::path path =
      fs::path(folder) / ("plegacy-" + std::string(stamp) + ".log");
  auto file =
      std::make_shared<std::ofstream>(path, std::ios::out | std::ios::trunc);
  if (!*file) {
    return {};
  }
  add_sink(
      [file](LogLevel lvl, const std::string& msg) {
        // Flushed per line: the log is for finding out what happened
        // before a crash.
        *file << level_prefix(lvl) << msg << '\n' << std::flush;
      },
      LogLevel::Info);
  return path.string();
}

void Logger::log(LogLevel lvl, const std::string& msg) {
  std::lock_guard lock(log_mutex());
  LogLevel lowest = min_lvl_();
  for (const auto& s : sinks_()) {
    lowest = std::min(lowest, s.min.value_or(min_lvl_()));
  }
  if (lvl < lowest) {
    return;
  }

  auto now = Clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - start_time_());
  auto delta =
      std::chrono::duration_cast<std::chrono::microseconds>(now - last_time_());
  last_time_() = now;

  auto elapsed_s = elapsed.count() / 1000;
  auto elapsed_ms = elapsed.count() % 1000;
  auto delta_ms = delta.count() / 1000;
  auto delta_us = delta.count() % 1000;

  auto stamped = compat::format("[{}.{:03d} +{}.{:03d}ms] {}", elapsed_s,
                                elapsed_ms, delta_ms, delta_us, msg);

  // A copy: a sink that logs must not see the list change under it.
  const auto sinks = sinks_();
  for (const auto& s : sinks) {
    if (lvl >= s.min.value_or(min_lvl_())) {
      s.func(lvl, stamped);
    }
  }
}

std::string Logger::level_prefix(LogLevel lvl) {
  switch (lvl) {
  case LogLevel::Debug:
    return "(DEBUG) ";
  case LogLevel::Info:
    return "(INFO) ";
  case LogLevel::Warning:
    return "(WARN) ";
  }
  return {};
}
