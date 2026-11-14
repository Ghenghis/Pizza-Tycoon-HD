/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace core::content {

// SHA-256 (FIPS 180-4), for the content hashes the location contract pins
// (definition_sha256, save roster entries). Not for anything secret.
class Sha256 {
public:
  Sha256();

  void update(std::string_view bytes);
  // The digest of everything passed to update. The object is spent after it.
  [[nodiscard]] std::array<std::uint8_t, 32> finish();

  // Lower-case hex of the digest of bytes.
  [[nodiscard]] static std::string hex(std::string_view bytes);

private:
  void block(const std::uint8_t* data);

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t length_{0};
};

} // namespace core::content
