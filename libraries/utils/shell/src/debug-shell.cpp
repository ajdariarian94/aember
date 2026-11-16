/**
 * @file debug-shell.cpp
 * @author Arian Ajdari
 * @brief Debug shell on the console without delaying boot.
 * @version 0.2
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/shell/debug-shell.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace aember::utils::shell {

namespace {

constexpr auto kAlivePoll = std::chrono::milliseconds{200};

/// Console in single-key mode (no line buffering, no echo); restores the
/// previous settings on destruction.
class RawConsole {
 public:
  explicit RawConsole(int fd) : fd_(fd) {
    if (tcgetattr(fd_, &saved_) != 0) return;
    termios raw = saved_;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(fd_, TCSANOW, &raw) == 0;
  }
  ~RawConsole() {
    if (active_) tcsetattr(fd_, TCSANOW, &saved_);
  }
  RawConsole(const RawConsole&) = delete;
  RawConsole& operator=(const RawConsole&) = delete;

 private:
  int fd_;
  termios saved_{};
  bool active_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

DebugShell::DebugShell() : log_("debug-shell") {
  if (::pipe2(wake_pipe_, O_CLOEXEC) != 0) {
    log_.error("pipe2() failed: {}", strerror(errno));
  }
}

DebugShell::~DebugShell() {
  Stop();
  for (const int fd : wake_pipe_) {
    if (fd >= 0) ::close(fd);
  }
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void DebugShell::Start(OnSpawn on_spawn, IsAlive is_alive, bool shell_now,
                       const std::string& console) {
  if (thread_.joinable()) return;
  on_spawn_ = std::move(on_spawn);
  is_alive_ = std::move(is_alive);

  thread_ = std::jthread{[this, console, shell_now](std::stop_token st) {
    // A throwing thread would reach std::terminate: never in PID 1.
    try {
      Listen(std::move(st), console, shell_now);
    } catch (const std::exception& e) {
      log_.error("Debug shell listener failed: {}", e.what());
    } catch (...) { log_.error("Debug shell listener failed"); }
  }};
}

void DebugShell::Stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  if (wake_pipe_[1] >= 0) {
    const char byte = 0;
    std::ignore = ::write(wake_pipe_[1], &byte, 1);
  }
  thread_.join();
}

// ---------------------------------------------------------------------------
// Kernel command line
// ---------------------------------------------------------------------------

bool DebugShell::RequestedOnCmdline(const std::string& path) {
  std::ifstream file{path};
  std::string token;
  while (file >> token) {
    if (token == "aember.shell=1" || token == "aember.shell=yes" ||
        token == "aember.shell=true") {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

void DebugShell::Listen(std::stop_token st, const std::string& console,
                        bool shell_now) {
  if (shell_now) {
    log_.warn("Debug shell requested at boot");
    if (const pid_t pid = SpawnShell(console); pid > 0) {
      WaitForShell(st, pid);
    }
  }

  while (!st.stop_requested()) {
    const int fd = ::open(console.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
      log_.error(
          "Cannot open {}: {}; no debug shell", console, strerror(errno));
      return;
    }

    log_.info("Press 'd' on the console for a debug shell");

    bool requested = false;
    {
      RawConsole raw{fd};
      while (!st.stop_requested() && !requested) {
        pollfd fds[2] = {{fd, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
          if (errno == EINTR) continue;
          log_.error("poll() failed: {}", strerror(errno));
          break;
        }
        if (fds[1].revents != 0) break;  // Stop()
        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          // Hangup or error on the console: back off instead of spinning.
          std::this_thread::sleep_for(std::chrono::seconds{1});
          break;
        }
        char c = 0;
        if (::read(fd, &c, 1) == 1 && (c == 'd' || c == 'D')) {
          requested = true;
        }
      }
    }  // console settings restored before the shell starts
    ::close(fd);

    if (!requested) continue;

    log_.warn("Debug shell requested");
    if (const pid_t pid = SpawnShell(console); pid > 0) {
      WaitForShell(st, pid);
    }
  }
}

pid_t DebugShell::SpawnShell(const std::string& console) {
  // PID 1's log lines would interleave with the shell; the file log keeps
  // everything.
  aember::utils::logging::set_console_logging(false);

  const pid_t pid = ::fork();
  if (pid == 0) {
    // Own session with the console as controlling tty: job control and
    // Ctrl-C work in the shell.
    ::setsid();
    const int fd = ::open(console.c_str(), O_RDWR);
    if (fd < 0) ::_exit(127);
    ::ioctl(fd, TIOCSCTTY, 1);
    ::dup2(fd, STDIN_FILENO);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) ::close(fd);

    sigset_t all;
    sigfillset(&all);
    ::sigprocmask(SIG_UNBLOCK, &all, nullptr);

    ::execl("/bin/sh", "sh", nullptr);
    ::_exit(127);
  }

  if (pid < 0) {
    log_.error("fork() failed: {}", strerror(errno));
    aember::utils::logging::set_console_logging(true);
    return -1;
  }

  if (on_spawn_) on_spawn_(pid);
  return pid;
}

void DebugShell::WaitForShell(std::stop_token st, pid_t pid) {
  // Without a reaper (is_alive_ unset) collect the shell ourselves.
  const auto alive = [&] {
    if (is_alive_) return is_alive_(pid);
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == 0;
  };
  while (!st.stop_requested() && alive()) {
    pollfd wake{wake_pipe_[0], POLLIN, 0};
    if (::poll(&wake, 1, static_cast<int>(kAlivePoll.count())) > 0) break;
  }
  if (st.stop_requested()) return;

  aember::utils::logging::set_console_logging(true);
  log_.info("Debug shell exited");
}

}  // namespace aember::utils::shell
