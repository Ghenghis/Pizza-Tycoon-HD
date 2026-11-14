/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace views::hd {

// Builds one thing off the game thread, so a view never waits for it: the
// view asks with start(), keeps drawing what it has, and picks the result up
// with take() on a later frame. Results are keyed by what they were built
// from: starting a job for another key stops the old one, and whatever it
// still produces is dropped, never handed over.
//
// One std::jthread per job. A stopped job's thread finishes on its own; it
// is joined once it has said so (take() and start() look), or at the latest
// by the destructor, which also asks it to stop. The work should check its
// stop_token often (once per row of a picture), so that is quick.
template <typename T> class BackgroundJob {
public:
  // Returns the result, or null when it was stopped or failed.
  using Work = std::function<std::shared_ptr<T>(std::stop_token)>;

  BackgroundJob() = default;
  BackgroundJob(const BackgroundJob&) = delete;
  BackgroundJob& operator=(const BackgroundJob&) = delete;
  ~BackgroundJob() {
    retire_current();
    // Every thread is asked first, then each is joined, so they stop
    // together instead of one after the other.
    for (Run& r : retired_) {
      r.thread.request_stop();
    }
    retired_.clear(); // joins
  }

  // Builds `key` with `work` on a worker, unless this job is already on
  // `key` (running, or done and not yet taken). A job for another key is
  // stopped first, and its result dropped.
  void start(uint64_t key, Work work) {
    reap();
    if (current_ && current_->key == key) {
      return;
    }
    retire_current();
    auto shared = std::make_shared<Shared>();
    current_.emplace(
        key, shared,
        std::jthread([shared, fn = std::move(work)](std::stop_token stop) {
          std::shared_ptr<T> result;
          try {
            result = fn(stop);
          }
          catch (...) {
            // Out of memory for a 4K picture, say: no picture, not the end
            // of the game (an exception leaving a thread would be).
            result.reset();
          }
          std::lock_guard lock(shared->mutex);
          if (!stop.stop_requested()) {
            shared->result = std::move(result);
          }
          shared->done = true;
        }));
  }

  // The result for the key last started, once, when it is ready; null
  // before (and after it was taken, and when the work gave nothing).
  [[nodiscard]] std::shared_ptr<T> take() {
    reap();
    if (!current_) {
      return nullptr;
    }
    std::shared_ptr<T> result;
    {
      std::lock_guard lock(current_->shared->mutex);
      if (!current_->shared->done) {
        return nullptr;
      }
      result = std::move(current_->shared->result);
    }
    // Finished: its thread is leaving, join it later rather than now.
    retire_current();
    return result;
  }

  // True from start() until the result was taken or the job cancelled.
  [[nodiscard]] bool started() const noexcept { return current_.has_value(); }

  // Stops the current job, if any; its result will be dropped.
  void cancel() { retire_current(); }

private:
  struct Shared {
    std::mutex mutex;
    std::shared_ptr<T> result;
    bool done = false;
  };
  struct Run {
    Run(uint64_t k, std::shared_ptr<Shared> s, std::jthread t)
        : key(k), shared(std::move(s)), thread(std::move(t)) {}
    uint64_t key;
    std::shared_ptr<Shared> shared;
    std::jthread thread;
  };

  void retire_current() {
    if (current_) {
      current_->thread.request_stop();
      retired_.push_back(std::move(*current_));
      current_.reset();
    }
  }

  // Joins the stopped jobs that have finished (their join is immediate).
  void reap() {
    std::erase_if(retired_, [](Run& r) {
      std::lock_guard lock(r.shared->mutex);
      return r.shared->done;
    });
  }

  std::optional<Run> current_;
  std::vector<Run> retired_;
};

} // namespace views::hd
