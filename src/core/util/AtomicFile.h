/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace util {

// Replaces a file so that whoever reads it next, including the game after a
// crash or a power cut, finds either the old contents or the new ones, never
// a mix of the two and never an empty file.
//
// The new bytes go to a temporary file beside the destination. commit()
// flushes that file to the disk, closes it and renames it over the
// destination in one step. Until then the destination is not touched. If any
// step fails, or the AtomicFile goes away without a commit, the temporary
// file is removed and the destination keeps what it had.
//
// A destination that is a symbolic link has its target replaced, so the link
// stays a link.
class AtomicFile {
public:
  explicit AtomicFile(std::filesystem::path destination);
  ~AtomicFile();

  AtomicFile(const AtomicFile&) = delete;
  AtomicFile& operator=(const AtomicFile&) = delete;
  AtomicFile(AtomicFile&&) = delete;
  AtomicFile& operator=(AtomicFile&&) = delete;

  // Creates the temporary file. Returns false if it could not.
  [[nodiscard]] bool open();

  // Appends to the temporary file. Returns false if this or any earlier
  // step failed.
  [[nodiscard]] bool write(const void* data, std::size_t size);
  [[nodiscard]] bool write(std::string_view text);

  // Makes the new contents the destination's. Returns false, with the
  // destination as it was, if any step failed.
  [[nodiscard]] bool commit();

  // Throws the new contents away. The destination keeps its old ones.
  void discard();

  [[nodiscard]] bool failed() const { return !error_.empty(); }

  // What went wrong, for the log: the step, the path and the system's
  // reason. Empty while nothing has failed.
  [[nodiscard]] const std::string& error() const { return error_; }

  // The system error behind error(). Disk full and access denied compare
  // equal to std::errc::no_space_on_device and std::errc::permission_denied
  // on every platform.
  [[nodiscard]] std::error_code error_code() const { return error_code_; }

  [[nodiscard]] const std::filesystem::path& destination() const {
    return destination_;
  }

  // The temporary file while one is open; empty otherwise.
  [[nodiscard]] const std::filesystem::path& temp_path() const {
    return temp_path_;
  }

private:
  // Records the first failure and cleans up. Always returns false.
  bool fail(std::string_view step, const std::filesystem::path& path,
            std::error_code code, std::string_view reason);
  void close_temp();
  void remove_temp();

  std::filesystem::path destination_;
  // What the rename replaces: the destination, or its target if it is a link.
  std::filesystem::path target_;
  std::filesystem::path temp_path_;
  std::string error_;
  std::error_code error_code_;
  bool committed_ = false;
#ifdef _WIN32
  void* handle_ = nullptr; // a Windows HANDLE while the temporary file is open
#else
  int fd_ = -1;
#endif
};

struct AtomicWriteResult {
  std::string error; // empty on success, as AtomicFile::error()
  std::error_code code;

  explicit operator bool() const { return error.empty(); }
};

// Writes contents to destination through an AtomicFile.
[[nodiscard]] AtomicWriteResult
write_file_atomically(const std::filesystem::path& destination,
                      std::string_view contents);

} // namespace util
