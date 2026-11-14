/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/util/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace util {

namespace fs = std::filesystem;

namespace {

// How many temporary names to try before giving up.
constexpr int temp_name_attempts = 16;

// The most handed to the system in one write call.
constexpr std::size_t max_write_chunk = std::size_t{1} << 30;

// A path as UTF-8, for messages.
std::string display(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::uint64_t process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

// Twelve hex digits that differ between processes and between calls.
std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::uint64_t x =
      (process_id() << 32) ^ ticks ^
      (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL);
  // The splitmix64 finaliser, so that neighbouring inputs give unrelated names.
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;

  constexpr std::string_view digits = "0123456789abcdef";
  std::string out(12, '0');
  for (char& c : out) {
    c = digits[x & 0xF];
    x >>= 4;
  }
  return out;
}

// Beside the target, so the final rename never crosses file systems.
fs::path temp_name_for(const fs::path& target) {
  fs::path name = target.filename();
  name += ".tmp-";
  name += unique_suffix();
  return target.parent_path() / name;
}

#ifdef _WIN32

std::string windows_message(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text;
  if (length > 0 && buffer != nullptr) {
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                            nullptr, 0, nullptr, nullptr);
    if (size > 0) {
      text.resize(static_cast<std::size_t>(size));
      WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                          text.data(), size, nullptr, nullptr);
    }
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  if (text.empty()) {
    text = "system error";
  }
  return text + " (" + std::to_string(code) + ")";
}

// Disk full and access denied map onto the portable conditions, the rest
// keep their Windows code.
std::error_code windows_code(DWORD code) {
  switch (code) {
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    return std::make_error_code(std::errc::no_space_on_device);
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_WRITE_PROTECT:
    return std::make_error_code(std::errc::permission_denied);
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return std::make_error_code(std::errc::no_such_file_or_directory);
  default:
    return {static_cast<int>(code), std::system_category()};
  }
}

// MoveFileExW replaces the target in one step, and with WRITE_THROUGH it
// returns only once the move is on the disk. A virus scanner or indexer that
// holds the file open for a moment makes it fail with a sharing error, so a
// few short retries ride that out. Returns the last error, or ERROR_SUCCESS.
DWORD replace_file(const fs::path& from, const fs::path& to) {
  constexpr int attempts = 5;
  DWORD error = ERROR_SUCCESS;
  for (int i = 0; i < attempts; ++i) {
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return ERROR_SUCCESS;
    }
    error = GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
        error != ERROR_LOCK_VIOLATION) {
      break;
    }
    if (i + 1 < attempts) {
      Sleep(static_cast<DWORD>(10 << i));
    }
  }
  return error;
}

#else

std::error_code errno_code(int error) {
#ifdef EDQUOT
  // Over quota is a full disk as far as the player is concerned.
  if (error == EDQUOT) {
    return std::make_error_code(std::errc::no_space_on_device);
  }
#endif
  return {error, std::generic_category()};
}

std::string errno_message(int error) {
  return std::generic_category().message(error);
}

// Gives the new file the old one's permissions. Best effort: if it fails the
// new file has the default ones, which is what a first save gets anyway.
void copy_permissions(int fd, const fs::path& from) {
  struct stat info {};
  if (::stat(from.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return;
  }
  static_cast<void>(::fchmod(fd, info.st_mode & 07777));
}

// Flushes the file's data to the disk. Returns 0 or the errno. A file system
// that cannot sync says EINVAL or ENOTSUP; the data still reaches it on
// close, so that is no failure.
int sync_file(int fd) {
#ifdef __APPLE__
  // fsync on macOS stops at the drive's cache; this goes all the way.
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
#endif
  while (::fsync(fd) != 0) {
    const int error = errno;
    if (error == EINTR) {
      continue;
    }
    return (error == EINVAL || error == ENOTSUP) ? 0 : error;
  }
  return 0;
}

// Makes the rename itself durable. Best effort: the new contents are already
// in place, and not every system lets a directory be opened and synced.
void sync_directory(const fs::path& dir) {
  const fs::path where = dir.empty() ? fs::path(".") : dir;
  const int fd = ::open(where.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  static_cast<void>(::fsync(fd));
  static_cast<void>(::close(fd));
}

#endif

} // namespace

AtomicFile::AtomicFile(fs::path destination)
    : destination_(std::move(destination)) {}

AtomicFile::~AtomicFile() {
  if (!committed_) {
    discard();
  }
}

bool AtomicFile::fail(std::string_view step, const fs::path& path,
                      std::error_code code, std::string_view reason) {
  if (error_.empty()) {
    error_ =
        std::string(step) + " " + display(path) + ": " + std::string(reason);
    error_code_ = code;
  }
  discard();
  return false;
}

bool AtomicFile::open() {
  if (failed()) {
    return false;
  }
  if (committed_ || !temp_path_.empty()) {
    return fail("cannot reopen", destination_,
                std::make_error_code(std::errc::operation_not_permitted),
                "an AtomicFile writes once");
  }

  // Replace a link's target, not the link. A dangling link is replaced
  // itself, as there is nothing else to replace.
  target_ = destination_;
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(destination_, ec))) {
    fs::path resolved = fs::canonical(destination_, ec);
    if (!ec) {
      target_ = std::move(resolved);
    }
  }

  for (int attempt = 0; attempt < temp_name_attempts; ++attempt) {
    fs::path candidate = temp_name_for(target_);
#ifdef _WIN32
    HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      handle_ = handle;
      temp_path_ = std::move(candidate);
      return true;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
      return fail("could not create a temporary file for", target_,
                  windows_code(error), windows_message(error));
    }
#else
    const int fd = ::open(candidate.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd >= 0) {
      fd_ = fd;
      temp_path_ = std::move(candidate);
      copy_permissions(fd_, target_);
      return true;
    }
    const int error = errno;
    if (error != EEXIST && error != EINTR) {
      return fail("could not create a temporary file for", target_,
                  errno_code(error), errno_message(error));
    }
#endif
  }
  return fail("could not create a temporary file for", target_,
              std::make_error_code(std::errc::file_exists),
              "every temporary name tried was taken");
}

bool AtomicFile::write(const void* data, std::size_t size) {
  if (failed()) {
    return false;
  }
  if (temp_path_.empty()) {
    return fail("cannot write", destination_,
                std::make_error_code(std::errc::bad_file_descriptor),
                "it was never opened");
  }

  const auto* bytes = static_cast<const unsigned char*>(data);
  while (size > 0) {
    const std::size_t chunk = std::min(size, max_write_chunk);
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes,
                   static_cast<DWORD>(chunk), &written, nullptr)) {
      const DWORD error = GetLastError();
      return fail("could not write", temp_path_, windows_code(error),
                  windows_message(error));
    }
    if (written == 0) {
      return fail("could not write", temp_path_,
                  std::make_error_code(std::errc::io_error),
                  "the system wrote nothing");
    }
#else
    const ssize_t written = ::write(fd_, bytes, chunk);
    if (written < 0) {
      const int error = errno;
      if (error == EINTR) {
        continue;
      }
      return fail("could not write", temp_path_, errno_code(error),
                  errno_message(error));
    }
    if (written == 0) {
      return fail("could not write", temp_path_,
                  std::make_error_code(std::errc::io_error),
                  "the system wrote nothing");
    }
#endif
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

bool AtomicFile::write(std::string_view text) {
  return write(text.data(), text.size());
}

bool AtomicFile::commit() {
  if (failed()) {
    return false;
  }
  if (committed_) {
    return true;
  }
  if (temp_path_.empty()) {
    return fail("cannot write", destination_,
                std::make_error_code(std::errc::bad_file_descriptor),
                "it was never opened");
  }

#ifdef _WIN32
  auto* handle = static_cast<HANDLE>(std::exchange(handle_, nullptr));
  if (!FlushFileBuffers(handle)) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    return fail("could not flush", temp_path_, windows_code(error),
                windows_message(error));
  }
  if (!CloseHandle(handle)) {
    const DWORD error = GetLastError();
    return fail("could not close", temp_path_, windows_code(error),
                windows_message(error));
  }
  const DWORD error = replace_file(temp_path_, target_);
  if (error != ERROR_SUCCESS) {
    return fail("could not replace", target_, windows_code(error),
                windows_message(error));
  }
#else
  if (const int error = sync_file(fd_); error != 0) {
    return fail("could not flush", temp_path_, errno_code(error),
                errno_message(error));
  }
  // The descriptor is gone whatever close says. EINTR leaves the data
  // written, as the sync above already went through.
  if (::close(std::exchange(fd_, -1)) != 0 && errno != EINTR) {
    const int error = errno;
    return fail("could not close", temp_path_, errno_code(error),
                errno_message(error));
  }
  std::error_code ec;
  fs::rename(temp_path_, target_, ec);
  if (ec) {
    return fail("could not replace", target_, ec, ec.message());
  }
#endif

  temp_path_.clear();
  committed_ = true;
#ifndef _WIN32
  sync_directory(target_.parent_path());
#endif
  return true;
}

void AtomicFile::discard() {
  close_temp();
  remove_temp();
}

void AtomicFile::close_temp() {
#ifdef _WIN32
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
#endif
}

void AtomicFile::remove_temp() {
  if (temp_path_.empty()) {
    return;
  }
  std::error_code ec;
  fs::remove(temp_path_, ec);
  temp_path_.clear();
}

AtomicWriteResult write_file_atomically(const fs::path& destination,
                                        std::string_view contents) {
  AtomicFile file(destination);
  if (file.open() && file.write(contents) && file.commit()) {
    return {};
  }
  return {file.error(), file.error_code()};
}

} // namespace util
