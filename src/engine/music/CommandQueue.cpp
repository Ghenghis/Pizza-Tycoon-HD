/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/CommandQueue.h"

#include <algorithm>
#include <array>
#include <utility>

namespace music {

void CommandQueue::push(Command command) {
  if (items_.size() < CAPACITY) {
    items_.push_back(std::move(command));
    return;
  }
  ++coalesced_;
  const auto same =
      std::find_if(items_.rbegin(), items_.rend(),
                   [&](const Command& c) { return c.kind == command.kind; });
  if (same != items_.rend()) {
    *same = std::move(command);
    return;
  }
  // None of its kind: keep the newest of each kind, in their order.
  std::array<bool, 3> seen{};
  std::vector<Command> kept;
  for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
    bool& s = seen[static_cast<std::size_t>(it->kind)];
    if (!s) {
      s = true;
      kept.push_back(std::move(*it));
    }
  }
  std::reverse(kept.begin(), kept.end());
  items_ = std::move(kept);
  items_.push_back(std::move(command));
}

std::vector<Command> CommandQueue::take() {
  std::vector<Command> out;
  out.swap(items_);
  items_.reserve(CAPACITY);
  return out;
}

} // namespace music
