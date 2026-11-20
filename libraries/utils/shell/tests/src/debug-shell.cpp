/**
 * @file debug-shell.cpp
 * @author Arian Ajdari
 * @brief DebugShell tests on a pseudo-terminal standing in for the console.
 * @version 0.1
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/shell/debug-shell.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using aember::utils::shell::DebugShell;
using namespace std::chrono_literals;

/// A pty pair: the test types on the master, DebugShell uses the slave as
/// its console.
class Pty {
 public:
  Pty() {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master_ >= 0 && ::grantpt(master_) == 0 && ::unlockpt(master_) == 0) {
      slave_ = ::ptsname(master_);
    }
  }
  ~Pty() {
    if (master_ >= 0) ::close(master_);
  }
  bool ok() const { return !slave_.empty(); }
  const std::string& slave() const { return slave_; }
  void Type(const std::string& s) const {
    std::ignore = ::write(master_, s.data(), s.size());
  }
  /// Read everything the pty prints until @p needle shows up or timeout.
  bool WaitFor(const std::string& needle, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd p{master_, POLLIN, 0};
      if (::poll(&p, 1, 50) > 0) {
        char buf[256];
        const ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n > 0) output_.append(buf, static_cast<size_t>(n));
      }
      if (output_.find(needle) != std::string::npos) return true;
    }
    return false;
  }

 private:
  int master_ = -1;
  std::string slave_;
  std::string output_;
};

std::string WriteCmdline(const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("aember-cmdline-" + std::to_string(::getpid()));
  std::ofstream{path} << content;
  return path.string();
}

TEST(DebugShellTest, CmdlineFlag) {
  EXPECT_TRUE(DebugShell::RequestedOnCmdline(
      WriteCmdline("console=ttyS0 root=/dev/vda1 aember.shell=1 rw\n")));
  EXPECT_TRUE(DebugShell::RequestedOnCmdline(WriteCmdline("aember.shell=yes")));
  EXPECT_FALSE(DebugShell::RequestedOnCmdline(
      WriteCmdline("console=ttyS0 aember.shell=0 aember.shellx=1")));
  EXPECT_FALSE(DebugShell::RequestedOnCmdline("/nonexistent/cmdline"));
}

// No key, no shell: Start() returns at once and nothing is spawned.
TEST(DebugShellTest, StartDoesNotBlockOrSpawn) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  std::atomic<int> spawned{0};

  DebugShell shell;
  const auto t0 = std::chrono::steady_clock::now();
  shell.Start([&](pid_t) { ++spawned; }, nullptr, false, pty.slave());
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 100ms);

  std::this_thread::sleep_for(300ms);
  pty.Type("x");  // other keys are ignored
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(spawned.load(), 0);
  shell.Stop();
}

// 'd' at any time opens a shell on the console; after it exits, 'd' opens
// another one.
TEST(DebugShellTest, KeyOpensShellRepeatedly) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  std::atomic<int> spawned{0};

  DebugShell shell;
  shell.Start([&](pid_t) { ++spawned; }, nullptr, false, pty.slave());

  for (int round = 1; round <= 2; ++round) {
    std::this_thread::sleep_for(200ms);
    pty.Type("d");
    const std::string marker = "shell-round-" + std::to_string(round);
    // Split so the echo of the typed command doesn't match.
    pty.Type("echo shell-round-\"\"" + std::to_string(round) + "; exit\n");
    EXPECT_TRUE(pty.WaitFor(marker, 5s)) << "round " << round;
    // Wait until the listener noticed the exit and listens again.
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(spawned.load(), round);
  }
  shell.Stop();
}

// shell_now (aember.shell=1, emergency shell) opens it without a key.
TEST(DebugShellTest, ShellNow) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  std::atomic<int> spawned{0};

  DebugShell shell;
  shell.Start([&](pid_t) { ++spawned; }, nullptr, true, pty.slave());
  pty.Type("echo now-\"\"open; exit\n");
  EXPECT_TRUE(pty.WaitFor("now-open", 5s));
  EXPECT_EQ(spawned.load(), 1);
  shell.Stop();
}

// The console callback (PID 1 mutes its daemons with it) sees the shell
// open and close, and the own console log is muted in between.
TEST(DebugShellTest, ConsoleCallbackFollowsTheShell) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  std::mutex mutex;
  std::vector<bool> events;
  std::vector<bool> logging_during;

  DebugShell shell;
  shell.SetConsoleCallback([&](bool open) {
    std::lock_guard lock{mutex};
    events.push_back(open);
    logging_during.push_back(aember::utils::logging::console_logging_enabled());
  });
  shell.Start([](pid_t) {}, nullptr, true, pty.slave());
  pty.Type("echo cb-\"\"done; exit\n");
  EXPECT_TRUE(pty.WaitFor("cb-done", 5s));
  for (int i = 0; i < 50; ++i) {
    {
      std::lock_guard lock{mutex};
      if (events.size() >= 2) break;
    }
    std::this_thread::sleep_for(50ms);
  }
  shell.Stop();

  std::lock_guard lock{mutex};
  EXPECT_EQ(events, (std::vector<bool>{true, false}));
  EXPECT_EQ(logging_during, (std::vector<bool>{false, true}));
}

TEST(DebugShellTest, MissingConsoleEndsTheListenerQuietly) {
  DebugShell shell;
  shell.Start([](pid_t) { ADD_FAILURE() << "no shell without a console"; },
              nullptr,
              false,
              "/nonexistent/aember-console");
  std::this_thread::sleep_for(200ms);
  shell.Stop();  // the listener already gave up: Stop() must not hang
}

TEST(DebugShellTest, ThrowingConsoleCallbackDoesNotKillTheListener) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  std::atomic<int> calls{0};
  DebugShell shell;
  shell.SetConsoleCallback([&](bool) {
    ++calls;
    throw std::runtime_error("callback broke");
  });
  shell.Start([](pid_t) {}, nullptr, true, pty.slave());
  pty.Type("echo still-\"\"alive; exit\n");
  EXPECT_TRUE(pty.WaitFor("still-alive", 5s));
  for (int i = 0; i < 50 && calls.load() < 2; ++i) {
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_EQ(calls.load(), 2);  // shell opened and closed, both caught
  // Console logging is back on even though the callback threw.
  EXPECT_TRUE(aember::utils::logging::console_logging_enabled());
  shell.Stop();
}

TEST(DebugShellTest, HangupOnTheConsoleIsSurvived) {
  auto pty = std::make_unique<Pty>();
  ASSERT_TRUE(pty->ok());
  const std::string console = pty->slave();
  DebugShell shell;
  shell.Start([](pid_t) {}, nullptr, false, console);
  std::this_thread::sleep_for(200ms);
  pty.reset();                          // master closed: POLLHUP on the console
  std::this_thread::sleep_for(1500ms);  // backs off instead of spinning
  shell.Stop();
}

}  // namespace
