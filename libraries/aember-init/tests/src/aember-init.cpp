/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @brief Integration tests: AemberInit::StartRoot() with real services, run
 *        in a forked child (chroot mode, a temporary config/log directory,
 *        a pty as the console) and stopped with the shutdown signals.
 *
 *        Each test forks, so init's signal mask, threads and global logger
 *        never touch the test process. As root the child first enters a
 *        private mount namespace: StartRoot() mounts tmpfs over /tmp and
 *        /run, which must not reach the dev container (docker.sock, X11).
 *        As another user the mounts fail, are logged and init carries on,
 *        which is a scenario of its own.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/aember-init/pid1.h>
#include <aember-libs/utils/logging/logging.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace init = aember::aember_init;
using Action = init::pid1::ShutdownAction;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

std::string ReadFile(const fs::path& path) {
  std::ifstream file{path};
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

void WriteFile(const fs::path& path, const std::string& text) {
  std::ofstream{path} << text;
}

/// Poll @p pred every 50 ms until it holds or @p timeout passes.
bool WaitFor(const std::function<bool()>& pred,
             std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(50ms);
  }
  return pred();
}

int CountLines(const fs::path& path) {
  std::ifstream file{path};
  int n = 0;
  for (std::string line; std::getline(file, line);) ++n;
  return n;
}

/// Run @p fn in a forked child; "" means it passed, anything else is the
/// failure message. A child that hangs is killed after @p timeout.
std::string InChild(const std::function<std::string()>& fn,
                    std::chrono::seconds timeout = 60s) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) return "pipe() failed";
  const pid_t pid = ::fork();
  if (pid < 0) return "fork() failed";
  if (pid == 0) {
    ::close(pipe_fds[0]);
    std::string result;
    if (::geteuid() == 0 &&
        (::unshare(CLONE_NEWNS) != 0 ||
         ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)) {
      result = "no private mount namespace";
    } else {
      // Init logs a lot at debug level; keep the test output readable.
      const int null_fd = ::open("/dev/null", O_WRONLY);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      try {
        result = fn();
      } catch (const std::exception& e) {
        result = std::string{"exception: "} + e.what();
      }
    }
    std::ignore = ::write(pipe_fds[1], result.data(), result.size());
    FlushCoverage();
    ::_exit(result.empty() ? 0 : 1);
  }
  ::close(pipe_fds[1]);

  std::string message;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= 0ms) {
      ::kill(pid, SIGKILL);
      message = "timeout; " + message;
      break;
    }
    pollfd pfd{pipe_fds[0], POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) continue;
    char buf[512];
    const ssize_t n = ::read(pipe_fds[0], buf, sizeof buf);
    if (n <= 0) break;
    message.append(buf, static_cast<size_t>(n));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (message.empty() && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    message = "child died (status " + std::to_string(status) + ")";
  }
  return message;
}

/// A temporary config + log directory and a pty for the console. Not
/// under /tmp or /run: as root StartRoot() mounts tmpfs over those.
struct Sandbox {
  fs::path dir;
  int master = -1;
  std::string console;

  Sandbox() {
    std::string tmpl = "/var/tmp/aember-init-test-XXXXXX";
    if (::mkdtemp(tmpl.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    dir = tmpl;
    master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
      throw std::runtime_error("no pty");
    }
    console = ::ptsname(master);
  }
  ~Sandbox() {
    if (master >= 0) ::close(master);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  init::InitOptions Options() const {
    return {.chroot = true,
            .config_dir = dir.string(),
            .log_dir = dir.string(),
            .console = console};
  }

  /// A std::vector, not a json: nlohmann turns a one-element braced list
  /// into that element instead of an array (that's how this test once
  /// wrote "processes": {...}).
  void Services(const std::vector<nlohmann::json>& processes) const {
    auto list = nlohmann::json::array();
    for (const auto& p : processes) list.push_back(p);
    WriteFile(dir / "services.json",
              nlohmann::json{{"processes", list}}.dump(2));
  }

  /// A service running `sh -c <script>`; $D is the sandbox directory.
  nlohmann::json Shell(const std::string& name, const std::string& script,
                       nlohmann::json extra = nlohmann::json::object()) const {
    nlohmann::json p = {{"name", name},
                        {"executable", "/bin/sh"},
                        {"args", {"-c", script}},
                        {"environment", {{"D", dir.string()}}}};
    p.merge_patch(extra);
    return p;
  }

  nlohmann::json Monitor() const {
    try {
      return nlohmann::json::parse(ReadFile(dir / "aember-status.json"));
    } catch (...) { return {}; }
  }

  std::string Log() const { return ReadFile(dir / "aember-init.log"); }
};

/// Start init in this (child) process, run @p during on another thread,
/// then send @p signal and wait for StartRoot() to return. "" = passed.
std::string RunInit(Sandbox& box, int signal, Action expected,
                    const std::function<std::string()>& during) {
  init::pid1::BlockInitSignals();
  std::string during_error;
  init::AemberInit aember{"aember-init-test", box.Options()};
  std::jthread controller{[&] {
    during_error = during();
    ::kill(::getpid(), signal);
  }};
  aember.StartRoot();
  controller.join();
  if (!during_error.empty()) return during_error;
  if (aember.RequestedShutdown() != expected) {
    return std::string{"shutdown was "} +
           std::string{init::pid1::ToString(aember.RequestedShutdown())};
  }
  return "";
}

bool Alive(pid_t pid) {
  return pid > 0 && ::kill(pid, 0) == 0;
}

pid_t ReadPid(const fs::path& path) {
  try {
    return static_cast<pid_t>(std::stoi(ReadFile(path)));
  } catch (...) { return -1; }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(AemberInitTest, ConstructAndDestroyWithoutStarting) {
  // Stop() from the destructor must be a no-op when nothing ran.
  init::AemberInit aember{"aember-init-test"};
  aember.Stop();
  EXPECT_EQ(aember.RequestedShutdown(), Action::kNone);
}

// Regression: AemberInit registered only the first services.json entry
// (AddProcess ran inside a views::filter predicate that ranges::to evaluated
// twice; the second call failed as a duplicate).
TEST(AemberInitTest, ServicesStartInDependencyOrderAndStopOnPowerOff) {
  const auto error = InChild([] {
    Sandbox box;
    box.Services({
        // Listed first but depends on "base": must start after it.
        box.Shell("app",
                  "echo app >> $D/order; echo $$ > $D/app.pid; exec sleep 600",
                  {{"dependencies", {"base"}}}),
        box.Shell(
            "base",
            "echo base >> $D/order; echo $$ > $D/base.pid; exec sleep 600"),
    });
    pid_t app = -1, base = -1;
    auto result =
        RunInit(box, SIGUSR2, Action::kPowerOff, [&]() -> std::string {
          if (!WaitFor([&] { return CountLines(box.dir / "order") == 2; },
                       10s)) {
            return "services didn't start: " + ReadFile(box.dir / "order");
          }
          // Start order from init's log: without ipc a service counts as
          // running
          // once forked, so which shell writes first says nothing.
          const auto log = box.Log();
          const auto base_at = log.find("Started 'base'");
          const auto app_at = log.find("Started 'app'");
          if (base_at == std::string::npos || app_at == std::string::npos ||
              app_at < base_at) {
            return "wrong start order:\n" + log.substr(0, 3000);
          }
          app = ReadPid(box.dir / "app.pid");
          base = ReadPid(box.dir / "base.pid");
          if (!Alive(app) || !Alive(base)) return "services not running";
          return "";
        });
    if (!result.empty()) return result;
    // Stopped and reaped by init before StartRoot() returned.
    if (Alive(app) || Alive(base)) return std::string{"services still alive"};
    const auto log = box.Log();
    if (log.find("Shutdown requested: poweroff") == std::string::npos) {
      return std::string{"no shutdown in the log"};
    }
    if (log.find("Service 'base': starting -> running") == std::string::npos) {
      return "no state change in the log:\n" + log.substr(0, 2000);
    }
    return std::string{};
  });
  EXPECT_EQ(error, "");
}

struct SignalCase {
  int signal;
  Action action;
};

// Without a printer gtest dumps the struct's bytes, padding included, which
// valgrind reports as uninitialised (in every forked child, too).
void PrintTo(const SignalCase& c, std::ostream* os) {
  *os << ::strsignal(c.signal) << " -> " << init::pid1::ToString(c.action);
}

class AemberInitSignalTest : public ::testing::TestWithParam<SignalCase> {};

// BusyBox-compatible: SIGTERM/SIGINT reboot, SIGUSR2 power off, SIGUSR1 halt.
// Regression: SIGTERM called Stop() on the signal thread, which joined
// itself and crashed init.
TEST_P(AemberInitSignalTest, ShutdownSignalSetsTheAction) {
  const auto [sig, action] = GetParam();
  const auto error = InChild([sig, action] {
    Sandbox box;
    box.Services({});
    return RunInit(box, sig, action, [] {
      std::this_thread::sleep_for(300ms);
      return std::string{};
    });
  });
  EXPECT_EQ(error, "");
}

INSTANTIATE_TEST_SUITE_P(Signals, AemberInitSignalTest,
                         ::testing::Values(SignalCase{SIGTERM, Action::kReboot},
                                           SignalCase{SIGINT, Action::kReboot},
                                           SignalCase{SIGUSR2,
                                                      Action::kPowerOff},
                                           SignalCase{SIGUSR1, Action::kHalt}));

TEST(AemberInitTest, FailingServiceIsRestartedUpToMaxRestarts) {
  const auto error = InChild([] {
    Sandbox box;
    box.Services({box.Shell("flaky",
                            "echo run >> $D/runs; exit 3",
                            {{"restart_on_failure", true},
                             {"max_restarts", 2},
                             {"restart_delay_ms", 100}})});
    return RunInit(box, SIGUSR2, Action::kPowerOff, [&]() -> std::string {
      if (!WaitFor([&] { return CountLines(box.dir / "runs") >= 3; }, 15s)) {
        return "restarted " + std::to_string(CountLines(box.dir / "runs") - 1) +
               " times, want 2; log:\n" + box.Log().substr(0, 3000);
      }
      std::this_thread::sleep_for(1500ms);  // and not a third time
      if (const int runs = CountLines(box.dir / "runs"); runs != 3) {
        return "ran " + std::to_string(runs) + " times, want 3";
      }
      return "";
    });
  });
  EXPECT_EQ(error, "");
}

TEST(AemberInitTest, DaemonMessagesReachTheHeartbeat) {
  // An ipc service talks to init over fd 3 (one JSON message per packet):
  // readiness, then the network status and a container state, which the
  // heartbeat (aember-status.json) must show.
  const auto error = InChild([] {
    Sandbox box;
    const std::string script =
        R"(printf '{"type":"ready"}' >&3; )"
        R"(printf '{"type":"event","name":"network","data":{"online":true,"interface":"eth9"}}' >&3; )"
        R"(printf '{"type":"event","name":"container","data":{"name":"web","state":"running"}}' >&3; )"
        R"(printf 'not json' >&3; )"
        "exec sleep 600";
    box.Services({box.Shell(
        "netd", script, {{"ipc", true}, {"ready_timeout_ms", 20000}})});
    return RunInit(box, SIGUSR2, Action::kPowerOff, [&]() -> std::string {
      const auto has_all = [&] {
        const auto m = box.Monitor();
        if (!m.contains("network") ||
            m["network"].value("interface", "") != "eth9") {
          return false;
        }
        bool netd = false, web = false;
        for (const auto& s : m.value("services", nlohmann::json::array())) {
          netd |= s.value("name", "") == "netd" &&
                  s.value("state", "") == "running";
          web |= s.value("name", "") == "web" &&
                 s.value("type", "") == "container";
        }
        return netd && web;
      };
      if (!WaitFor(has_all, 10s)) {
        return "heartbeat: " + box.Monitor().dump() + "\nlog:\n" +
               box.Log().substr(0, 3000);
      }
      return "";
    });
  });
  EXPECT_EQ(error, "");
}

TEST(AemberInitTest, ServiceWithoutReadyIsTreatedAsUpAfterItsTimeout) {
  const auto error = InChild([] {
    Sandbox box;
    box.Services({
        box.Shell("silent",
                  "exec sleep 600",
                  {{"ipc", true}, {"ready_timeout_ms", 300}}),
        box.Shell("after",
                  "echo started > $D/after; exec sleep 600",
                  {{"dependencies", {"silent"}}}),
    });
    return RunInit(box, SIGUSR2, Action::kPowerOff, [&]() -> std::string {
      if (!WaitFor([&] { return fs::exists(box.dir / "after"); }, 10s)) {
        return std::string{"dependent never started"};
      }
      if (box.Log().find("didn't report ready in time") == std::string::npos) {
        return std::string{"no ready-timeout warning"};
      }
      return "";
    });
  });
  EXPECT_EQ(error, "");
}

TEST(AemberInitTest, MissingServicesFileStillBootsAndStops) {
  const auto error = InChild([] {
    Sandbox box;  // no services.json
    auto result = RunInit(box, SIGTERM, Action::kReboot, [&]() -> std::string {
      if (!WaitFor([&] { return box.Monitor().contains("services"); }, 10s)) {
        return "no heartbeat: " + box.Monitor().dump();
      }
      return "";
    });
    if (!result.empty()) return result;
    if (box.Log().find("Aember Init System stopped") == std::string::npos) {
      return std::string{"init did not stop cleanly"};
    }
    return std::string{};
  });
  EXPECT_EQ(error, "");
}

TEST(AemberInitTest, DebugShellOpensOnTheConsole) {
  const auto error = InChild([] {
    Sandbox box;
    box.Services({});
    return RunInit(box, SIGUSR2, Action::kPowerOff, [&]() -> std::string {
      std::string seen;
      const auto read_until = [&](const std::string& what) {
        return WaitFor(
            [&] {
              pollfd pfd{box.master, POLLIN, 0};
              while (::poll(&pfd, 1, 0) > 0) {
                char buf[256];
                const ssize_t n = ::read(box.master, buf, sizeof buf);
                if (n <= 0) break;
                seen.append(buf, static_cast<size_t>(n));
              }
              return seen.find(what) != std::string::npos;
            },
            10s);
      };
      // The listener opens the console once init is up.
      if (!WaitFor(
              [&] { return box.Log().find("Press 'd'") != std::string::npos; },
              10s)) {
        return std::string{"listener never started"};
      }
      std::ignore = ::write(box.master, "d", 1);
      std::this_thread::sleep_for(500ms);
      const std::string cmd = "echo shell-$((40+2)); exit\n";
      std::ignore = ::write(box.master, cmd.data(), cmd.size());
      if (!read_until("shell-42")) return "no shell output: " + seen;
      return std::string{};
    });
  });
  EXPECT_EQ(error, "");
}

}  // namespace
