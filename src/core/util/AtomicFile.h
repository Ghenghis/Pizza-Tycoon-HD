/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace util {

namespace testing {

// A fault a test can give chosen AtomicFile writes, to show on real files
// what each kind of failure leaves behind. The game never sets one.
struct AtomicFault {
  enum class Kind {
    // The write that would take the new file past `bytes` writes up to there
    // and fails, as it does when the disk fills up.
    DiskFull,
    // Everything reports success, but the file keeps only its first `bytes`
    // bytes: a write whose end the disk lost.
    LostTail,
    // The rename at the end fails, after the new bytes are flushed.
    RenameFails,
    // Stops just before the rename, as if the game were killed there:
    // commit() throws AtomicFileInterrupted and leaves the temporary file.
    Interrupted,
  };
  Kind kind = Kind::DiskFull;
  std::size_t bytes = 0;
};

// What an Interrupted fault throws. It is no std::exception, so no handler
// for ordinary failures takes it for one.
struct AtomicFileInterrupted {
  std::filesystem::path destination;
};

// While alive, the next `times` AtomicFile writes whose destination passes
// `matches` get the fault. One at a time, from one thread.
class ScopedAtomicFault {
public:
  using Matcher = std::function<bool(const std::filesystem::path&)>;

  ScopedAtomicFault(Matcher matches, AtomicFault fault, int times = 1);
  ~ScopedAtomicFault();

  ScopedAtomicFault(const ScopedAtomicFault&) = delete;
  ScopedAtomicFault& operator=(const ScopedAtomicFault&) = delete;

  // How many writes have had the fault so far.
  [[nodiscard]] int hits() const { return hits_; }

  // The fault for a write to destination, if one is set and it is due.
  [[nodiscard]] static std::optional<AtomicFault>
  take(const std::filesystem::path& destination);

private:
  Matcher matches_;
  AtomicFault fault_;
  int remaining_;
  int hits_ = 0;
  ScopedAtomicFault* previous_;
};

} // namespace testing

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
  bool write_to_temp(const unsigned char* bytes, std::size_t size);
  // A test's fault, applied just before the rename. Returns false, or
  // throws, if it stops the commit.
  bool apply_commit_fault();

  std::filesystem::path destination_;
  // What the rename replaces: the destination, or its target if it is a link.
  std::filesystem::path target_;
  std::filesystem::path temp_path_;
  std::string error_;
  std::error_code error_code_;
  bool committed_ = false;
  std::optional<testing::AtomicFault> fault_; // set by a test only
  std::size_t written_ = 0;
  bool keep_temp_ = false; // an interrupted commit leaves it, as a crash does
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

// Whether name is one AtomicFile gives its temporary files: the destination's
// name, ".tmp-", then twelve lowercase hex digits.
[[nodiscard]] bool is_temp_file_name(std::string_view name);

struct StaleTempFiles {
  std::vector<std::filesystem::path> removed;
  // What matched but could not be removed, or a folder that could not be
  // read, each with the reason.
  std::vector<std::pair<std::filesystem::path, std::string>> failed;
};

// Removes the temporary files that writes cut short by a crash or a power cut
// left in folder, without looking into its subfolders: regular files, not
// links, with a temporary file's name, last written at least min_age ago. A
// younger one may belong to a write still going on, in another copy of the
// game. A folder that isn't there has nothing to remove.
[[nodiscard]] StaleTempFiles
remove_stale_temp_files(const std::filesystem::path& folder,
                        std::chrono::seconds min_age);

} // namespace util
