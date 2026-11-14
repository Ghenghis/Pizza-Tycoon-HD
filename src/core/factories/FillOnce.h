/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <mutex>
#include <utility>

// Fills a table the first time any thread needs it. Only filling takes a
// lock; once the table is full, each later call is one atomic load, and the
// table is read without locks, as it never changes again. A fill that throws
// leaves it unfilled, for the next call to try again.
class FillOnce {
public:
  template <typename Fill> void ensure(Fill&& fill) {
    if (done_.load(std::memory_order_acquire)) {
      return;
    }
    std::call_once(once_, [&] {
      std::forward<Fill>(fill)();
      done_.store(true, std::memory_order_release);
    });
  }

private:
  std::once_flag once_;
  std::atomic<bool> done_{false};
};
