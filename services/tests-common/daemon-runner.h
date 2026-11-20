/**
 * @file daemon-runner.h
 * @author Arian Ajdari
 * @brief Test helper: run a service's Run<Name>() in a forked child the way
 *        PID 1 does (a SOCK_SEQPACKET channel on fd 3), read its messages,
 *        send it control messages and signals, collect its exit code.
 *
 *        As root the child gets private mount and network namespaces, so a
 *        daemon creating bridges or mounting never touches the dev
 *        container.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/ipc/channel.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember_test {

/// Coverage builds (AEMBER_COVERAGE, set by instrument.cmake): write this
/// process's gcov counters now. A forked child ends with _exit(), which
/// skips gcov's exit handler, so what it ran would never be counted. (A
/// weak reference doesn't work: it never pulls __gcov_dump out of
/// libgcov.a.) A no-op in other builds.
#ifdef AEMBER_COVERAGE
extern "C" void __gcov_dump(void);
inline void FlushCoverage() {
  __gcov_dump();
}
#else
inline void FlushCoverage() {}
#endif

class DaemonRunner {
 public:
  /// Fork and run @p daemon (returns the exit code) with the other end of a
  /// channel as fd 3. @p private_ns: as root, new mount + net namespaces.
  explicit DaemonRunner(const std::function<int()>& daemon,
                        bool private_ns = true) {
    auto pair = aember::utils::ipc::Channel::Pair();
    if (!pair) return;
    auto [ours, theirs] = std::move(*pair);
    pid_ = ::fork();
    if (pid_ == 0) {
      ours = {};
      if (private_ns && ::geteuid() == 0) {
        if (::unshare(CLONE_NEWNS | CLONE_NEWNET) != 0 ||
            ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
          ::_exit(120);
        }
      }
      const int fd = theirs.Release();
      if (fd != aember::utils::ipc::kInheritedFd) {
        ::dup2(fd, aember::utils::ipc::kInheritedFd);
        ::close(fd);
      }
      const int null_fd = ::open("/dev/null", O_WRONLY);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      int rc = 121;
      try {
        rc = daemon();
      } catch (...) { rc = 122; }
      FlushCoverage();
      ::_exit(rc);
    }
    channel_ = std::move(ours);
  }

  ~DaemonRunner() {
    if (pid_ > 0 && !exited_) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
  }

  DaemonRunner(const DaemonRunner&) = delete;
  DaemonRunner& operator=(const DaemonRunner&) = delete;

  [[nodiscard]] bool Started() const { return pid_ > 0; }
  pid_t Pid() const { return pid_; }
  aember::utils::ipc::Channel& Channel() { return channel_; }

  /// The next message of @p type ("ready", "event"), skipping others.
  std::optional<nlohmann::json> Expect(const std::string& type,
                                       std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      auto msg = channel_.Receive(left);
      if (!msg) {
        if (channel_.Closed()) return std::nullopt;
        continue;
      }
      if (aember::utils::ipc::TypeOf(*msg) == type) return msg;
    }
    return std::nullopt;
  }

  /// Signal the daemon and wait for its exit code (nullopt: still running
  /// after @p timeout, or killed by a signal).
  std::optional<int> Stop(int signal, std::chrono::milliseconds timeout) {
    ::kill(pid_, signal);
    return Wait(timeout);
  }

  std::optional<int> Wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t r = ::waitpid(pid_, &status, WNOHANG);
      if (r == pid_) {
        exited_ = true;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return std::nullopt;
      }
      ::usleep(20'000);
    }
    return std::nullopt;
  }

 private:
  pid_t pid_ = -1;
  bool exited_ = false;
  aember::utils::ipc::Channel channel_;
};

}  // namespace aember_test
