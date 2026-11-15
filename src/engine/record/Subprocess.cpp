/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/record/Subprocess.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace record {

namespace {

std::vector<std::string> with_second_input(std::vector<std::string> args,
                                           const std::string& where) {
  for (std::string& a : args) {
    if (a == Subprocess::SECOND_INPUT) {
      a = where;
    }
  }
  return args;
}

} // namespace

std::string
Subprocess::windows_command_line(const std::vector<std::string>& argv) {
  // The rules CommandLineToArgvW reads by: backslashes are literal unless
  // they come before a quote, where they double; an argument with a space,
  // a tab or a quote (or an empty one) is quoted.
  std::string line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (i > 0) {
      line += ' ';
    }
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) {
      line += a;
      continue;
    }
    line += '"';
    std::size_t slashes = 0;
    for (const char c : a) {
      if (c == '\\') {
        ++slashes;
        continue;
      }
      if (c == '"') {
        line.append(slashes * 2 + 1, '\\');
      }
      else {
        line.append(slashes, '\\');
      }
      slashes = 0;
      line += c;
    }
    line.append(slashes * 2, '\\'); // before the closing quote
    line += '"';
  }
  return line;
}

std::optional<fs::path>
find_program(const std::string& name,
             const std::optional<fs::path>& explicit_path) {
  std::error_code ec;
  if (explicit_path) {
    return fs::is_regular_file(*explicit_path, ec)
               ? std::optional<fs::path>(*explicit_path)
               : std::nullopt;
  }
  const char* path = std::getenv("PATH");
  if (!path) {
    return std::nullopt;
  }
#if defined(_WIN32)
  constexpr char sep = ';';
  const std::string file = name + ".exe";
#else
  constexpr char sep = ':';
  const std::string& file = name;
#endif
  const std::string all(path);
  std::size_t at = 0;
  while (at <= all.size()) {
    const std::size_t end = std::min(all.find(sep, at), all.size());
    const std::string dir = all.substr(at, end - at);
    at = end + 1;
    if (dir.empty()) {
      continue;
    }
    const fs::path candidate = fs::path(dir) / file;
    if (!fs::is_regular_file(candidate, ec)) {
      continue;
    }
#if !defined(_WIN32)
    if (::access(candidate.c_str(), X_OK) != 0) {
      continue;
    }
#endif
    return candidate;
  }
  return std::nullopt;
}

#if defined(_WIN32)

/* Windows */

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      w.data(), n);
  return w;
}

std::string last_error(const char* what) {
  return std::string(what) + " failed (Windows error " +
         std::to_string(GetLastError()) + ")";
}

void close_handle(HANDLE& h) {
  if (h && h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  h = nullptr;
}

} // namespace

struct Subprocess::Native {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE first = nullptr;  // write end of the child's stdin
  HANDLE second = nullptr; // our end of the named pipe (overlapped)
  HANDLE err = nullptr;    // read end of the child's stderr
  bool connected = false;
};

std::unique_ptr<Subprocess>
Subprocess::start(const fs::path& exe, const std::vector<std::string>& args,
                  std::string* why, std::size_t stderr_tail) {
  return start_impl(exe, args, why, stderr_tail, false);
}

std::unique_ptr<Subprocess>
Subprocess::start_impl(const fs::path& exe,
                       const std::vector<std::string>& args, std::string* why,
                       std::size_t stderr_tail, bool merge_stdout) {
  const auto fail = [why](std::string reason) -> std::unique_ptr<Subprocess> {
    if (why) {
      *why = std::move(reason);
    }
    return nullptr;
  };
  static std::atomic<unsigned> counter{0};
  const std::wstring pipe_name = L"\\\\.\\pipe\\pthd-rec-" +
                                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(counter.fetch_add(1)) + L"-" +
                                 std::to_wstring(GetTickCount64());

  std::unique_ptr<Subprocess> p(new Subprocess());
  p->native_ = std::make_unique<Native>();
  p->tail_limit_ = stderr_tail;
  Native& n = *p->native_;

  SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                              .lpSecurityDescriptor = nullptr,
                              .bInheritHandle = TRUE};
  HANDLE child_in = nullptr;
  HANDLE child_err = nullptr;
  HANDLE child_out = nullptr;
  const auto cleanup = [&] {
    close_handle(child_in);
    close_handle(child_err);
    if (child_out != child_err) {
      close_handle(child_out);
    }
  };
  if (!CreatePipe(&child_in, &n.first, &inherit, 1 << 20) ||
      !SetHandleInformation(n.first, HANDLE_FLAG_INHERIT, 0)) {
    cleanup();
    return fail(last_error("CreatePipe"));
  }
  if (!CreatePipe(&n.err, &child_err, &inherit, 0) ||
      !SetHandleInformation(n.err, HANDLE_FLAG_INHERIT, 0)) {
    cleanup();
    return fail(last_error("CreatePipe"));
  }
  n.second =
      CreateNamedPipeW(pipe_name.c_str(),
                       PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE |
                           FILE_FLAG_OVERLAPPED,
                       PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                       1, 1 << 20, 0, 0, nullptr);
  if (n.second == INVALID_HANDLE_VALUE) {
    n.second = nullptr;
    cleanup();
    return fail(last_error("CreateNamedPipe"));
  }
  child_out = merge_stdout
                  ? child_err
                  : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                &inherit, OPEN_EXISTING, 0, nullptr);
  if (child_out == INVALID_HANDLE_VALUE) {
    child_out = nullptr;
    cleanup();
    return fail(last_error("CreateFile NUL"));
  }

  // Only these three handles reach the child.
  std::vector<HANDLE> handles{child_in, child_err};
  if (child_out != child_err) {
    handles.push_back(child_out);
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<unsigned char> list_bytes(size);
  auto* list =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_bytes.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
      !UpdateProcThreadAttribute(
          list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
          handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
    cleanup();
    return fail(last_error("UpdateProcThreadAttribute"));
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = child_in;
  si.StartupInfo.hStdOutput = child_out;
  si.StartupInfo.hStdError = child_err;
  si.lpAttributeList = list;

  // Killed when the game's last handle to the job closes, even on a crash.
  n.job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!n.job ||
      !SetInformationJobObject(n.job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    DeleteProcThreadAttributeList(list);
    cleanup();
    return fail(last_error("CreateJobObject"));
  }

  std::vector<std::string> argv{exe.string()};
  for (std::string& a : with_second_input(
           args, std::string(pipe_name.begin(), pipe_name.end()))) {
    argv.push_back(std::move(a));
  }
  std::wstring line = wide(windows_command_line(argv));
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(
      exe.wstring().c_str(), line.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
          CREATE_UNICODE_ENVIRONMENT,
      nullptr, nullptr, &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(list);
  cleanup();
  if (!ok) {
    return fail(last_error("CreateProcess"));
  }
  if (!AssignProcessToJobObject(n.job, pi.hProcess)) {
    const std::string reason = last_error("AssignProcessToJobObject");
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return fail(reason);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  n.process = pi.hProcess;
  p->stderr_thread_ = std::thread([raw = p.get()] { raw->read_stderr(); });
  return p;
}

void Subprocess::read_stderr() {
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(native_->err, buffer, sizeof(buffer), &got, nullptr) &&
         got > 0) {
    const std::lock_guard lock(tail_mutex_);
    tail_.append(buffer, got);
    if (tail_.size() > tail_limit_) {
      tail_.erase(0, tail_.size() - tail_limit_);
    }
  }
}

bool Subprocess::write(Input input, std::span<const std::uint8_t> bytes) {
  Native& n = *native_;
  if (input == Input::FIRST) {
    while (!bytes.empty()) {
      DWORD done = 0;
      const auto chunk =
          static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 20));
      if (!n.first ||
          !WriteFile(n.first, bytes.data(), chunk, &done, nullptr)) {
        return false;
      }
      bytes = bytes.subspan(done);
    }
    return true;
  }
  if (!n.second) {
    return false;
  }
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) {
    return false;
  }
  bool ok = true;
  if (!n.connected) {
    // Waits for the child to open its end, or to end without doing so.
    if (!ConnectNamedPipe(n.second, &ov)) {
      const DWORD e = GetLastError();
      if (e == ERROR_IO_PENDING) {
        const HANDLE both[2] = {ov.hEvent, n.process};
        if (WaitForMultipleObjects(2, both, FALSE, INFINITE) != WAIT_OBJECT_0) {
          CancelIo(n.second);
          ok = false;
        }
      }
      else if (e != ERROR_PIPE_CONNECTED) {
        ok = false;
      }
    }
    n.connected = ok;
  }
  while (ok && !bytes.empty()) {
    DWORD done = 0;
    ResetEvent(ov.hEvent);
    const auto chunk =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 20));
    if (!WriteFile(n.second, bytes.data(), chunk, nullptr, &ov) &&
        GetLastError() != ERROR_IO_PENDING) {
      ok = false;
      break;
    }
    if (!GetOverlappedResult(n.second, &ov, &done, TRUE)) {
      ok = false;
      break;
    }
    bytes = bytes.subspan(done);
  }
  CloseHandle(ov.hEvent);
  return ok;
}

void Subprocess::close(Input input) {
  Native& n = *native_;
  if (input == Input::FIRST) {
    close_handle(n.first);
    return;
  }
  if (n.second && n.connected) {
    FlushFileBuffers(n.second); // what is written is read before it closes
  }
  close_handle(n.second);
}

std::optional<int> Subprocess::wait(std::chrono::milliseconds timeout) {
  if (exit_code_) {
    return exit_code_;
  }
  if (WaitForSingleObject(native_->process,
                          static_cast<DWORD>(timeout.count())) !=
      WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  GetExitCodeProcess(native_->process, &code);
  exit_code_ = static_cast<int>(code);
  if (stderr_thread_.joinable()) {
    stderr_thread_.join();
  }
  return exit_code_;
}

void Subprocess::kill() {
  if (native_->process && !exit_code_) {
    TerminateProcess(native_->process, 1);
  }
}

Subprocess::~Subprocess() {
  if (!native_) {
    return;
  }
  close(Input::FIRST);
  close(Input::SECOND);
  if (!wait(std::chrono::milliseconds(0))) {
    kill();
    wait(std::chrono::milliseconds(INFINITE));
  }
  if (stderr_thread_.joinable()) {
    stderr_thread_.join();
  }
  close_handle(native_->err);
  close_handle(native_->process);
  close_handle(native_->job);
}

#else

/* POSIX */

namespace {

bool make_pipe(int fds[2]) {
#if defined(__linux__)
  return ::pipe2(fds, O_CLOEXEC) == 0;
#else
  if (::pipe(fds) != 0) {
    return false;
  }
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
  }
  fd = -1;
}

std::string errno_text(const char* what) {
  return std::string(what) +
         " failed: " + std::system_category().message(errno);
}

} // namespace

struct Subprocess::Native {
  pid_t pid = -1;
  int first = -1; // write ends
  int second = -1;
  int err = -1; // read end
};

std::unique_ptr<Subprocess>
Subprocess::start(const fs::path& exe, const std::vector<std::string>& args,
                  std::string* why, std::size_t stderr_tail) {
  return start_impl(exe, args, why, stderr_tail, false);
}

std::unique_ptr<Subprocess>
Subprocess::start_impl(const fs::path& exe,
                       const std::vector<std::string>& args, std::string* why,
                       std::size_t stderr_tail, bool merge_stdout) {
  const auto fail = [why](std::string reason) -> std::unique_ptr<Subprocess> {
    if (why) {
      *why = std::move(reason);
    }
    return nullptr;
  };
  // A write to a child that has gone must fail, not end the game.
  static std::once_flag ignore_sigpipe;
  std::call_once(ignore_sigpipe, [] { std::signal(SIGPIPE, SIG_IGN); });

  int in1[2] = {-1, -1};
  int in2[2] = {-1, -1};
  int err[2] = {-1, -1};
  const auto close_all = [&] {
    for (int* fds : {in1, in2, err}) {
      close_fd(fds[0]);
      close_fd(fds[1]);
    }
  };
  if (!make_pipe(in1) || !make_pipe(in2) || !make_pipe(err)) {
    const std::string reason = errno_text("pipe");
    close_all();
    return fail(reason);
  }

  // Everything the child needs is made before it exists: after fork() only
  // async-signal-safe calls are allowed.
  const std::string path = exe.string();
  std::vector<std::string> strings{path};
  for (std::string& a : with_second_input(args, "pipe:3")) {
    strings.push_back(std::move(a));
  }
  std::vector<char*> argv;
  for (std::string& s : strings) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);

  pid_t pid = -1;
#if defined(__linux__)
  const pid_t parent = ::getpid();
  const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  pid = ::fork();
  if (pid == 0) {
    // Killed when the thread that started it ends; already gone if the
    // game ended before this line.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent) {
      ::_exit(127);
    }
    // Its own group, so a kill takes anything it started too.
    ::setpgid(0, 0);
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigaction(SIGPIPE, &dfl, nullptr);
    ::dup2(in1[0], 0);
    if (in2[0] == 3) {
      ::fcntl(3, F_SETFD, 0);
    }
    else {
      ::dup2(in2[0], 3);
    }
    ::dup2(merge_stdout ? err[1] : devnull, 1);
    ::dup2(err[1], 2);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  if (devnull >= 0) {
    ::close(devnull);
  }
  if (pid < 0) {
    const std::string reason = errno_text("fork");
    close_all();
    return fail(reason);
  }
#else
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_adddup2(&actions, in1[0], 0);
  posix_spawn_file_actions_adddup2(&actions, in2[0], 3);
  if (merge_stdout) {
    posix_spawn_file_actions_adddup2(&actions, err[1], 1);
  }
  else {
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  }
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setpgroup(&attr, 0); // its own group, signalled on close
  posix_spawnattr_setflags(&attr,
                           POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
  const int rc =
      posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    errno = rc;
    const std::string reason = errno_text("posix_spawn");
    close_all();
    return fail(reason);
  }
#endif
  close_fd(in1[0]);
  close_fd(in2[0]);
  close_fd(err[1]);

  std::unique_ptr<Subprocess> p(new Subprocess());
  p->native_ = std::make_unique<Native>();
  p->native_->pid = pid;
  p->native_->first = in1[1];
  p->native_->second = in2[1];
  p->native_->err = err[0];
  p->tail_limit_ = stderr_tail;
  p->stderr_thread_ = std::thread([raw = p.get()] { raw->read_stderr(); });
  return p;
}

void Subprocess::read_stderr() {
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::read(native_->err, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      return;
    }
    const std::lock_guard lock(tail_mutex_);
    tail_.append(buffer, static_cast<std::size_t>(got));
    if (tail_.size() > tail_limit_) {
      tail_.erase(0, tail_.size() - tail_limit_);
    }
  }
}

bool Subprocess::write(Input input, std::span<const std::uint8_t> bytes) {
  const int fd = input == Input::FIRST ? native_->first : native_->second;
  if (fd < 0) {
    return false;
  }
  while (!bytes.empty()) {
    const ssize_t done = ::write(fd, bytes.data(), bytes.size());
    if (done < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false; // EPIPE: the child no longer reads
    }
    bytes = bytes.subspan(static_cast<std::size_t>(done));
  }
  return true;
}

void Subprocess::close(Input input) {
  close_fd(input == Input::FIRST ? native_->first : native_->second);
}

std::optional<int> Subprocess::wait(std::chrono::milliseconds timeout) {
  if (exit_code_) {
    return exit_code_;
  }
  const auto until = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    int status = 0;
    const pid_t r = ::waitpid(native_->pid, &status, WNOHANG);
    if (r == native_->pid) {
      exit_code_ = WIFEXITED(status)     ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                         : -1;
      if (stderr_thread_.joinable()) {
        stderr_thread_.join();
      }
      return exit_code_;
    }
    if (r < 0 && errno != EINTR) {
      exit_code_ = -1;
      return exit_code_;
    }
    if (std::chrono::steady_clock::now() >= until) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void Subprocess::kill() {
  if (native_->pid > 0 && !exit_code_) {
    // The whole group: a process it started could hold its error pipe open.
    ::kill(-native_->pid, SIGKILL);
    ::kill(native_->pid, SIGKILL);
  }
}

Subprocess::~Subprocess() {
  if (!native_) {
    return;
  }
  close(Input::FIRST);
  close(Input::SECOND);
  if (!wait(std::chrono::milliseconds(0))) {
    kill();
    wait(std::chrono::hours(1));
  }
  if (stderr_thread_.joinable()) {
    stderr_thread_.join();
  }
  close_fd(native_->err);
}

#endif

std::string Subprocess::stderr_tail() const {
  const std::lock_guard lock(tail_mutex_);
  return tail_;
}

std::optional<Subprocess::Output>
Subprocess::run(const fs::path& exe, const std::vector<std::string>& args,
                std::chrono::milliseconds timeout, std::size_t max_output) {
  auto p = start_impl(exe, args, nullptr, max_output, true);
  if (!p) {
    return std::nullopt;
  }
  p->close(Input::FIRST);
  p->close(Input::SECOND);
  const auto code = p->wait(timeout);
  if (!code) {
    p->kill();
    return std::nullopt;
  }
  return Output{.exit_code = *code, .text = p->stderr_tail()};
}

} // namespace record
