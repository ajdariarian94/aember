/**
 * @file pid1.cpp
 * @author Arian Ajdari
 * @brief Tests for the PID 1 helpers that can run outside PID 1: shutdown
 *        action names and the init signal mask (blocked in init, unblocked
 *        again in fork() children).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/aember-init/pid1.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace pid1 = aember::aember_init::pid1;

constexpr int kInitSignals[] = {
    SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD};

TEST(Pid1Test, ShutdownActionNames) {
  std::set<std::string> names;
  for (const auto a : {pid1::ShutdownAction::kNone,
                       pid1::ShutdownAction::kReboot,
                       pid1::ShutdownAction::kPowerOff,
                       pid1::ShutdownAction::kHalt}) {
    names.emplace(pid1::ToString(a));
  }
  EXPECT_EQ(names.size(), 4u);
  EXPECT_EQ(pid1::ToString(pid1::ShutdownAction::kPowerOff), "poweroff");
  EXPECT_EQ(pid1::ToString(pid1::ShutdownAction::kReboot), "reboot");
}

/// Exit code of a child: 0 when all init signals are blocked (want_blocked)
/// or all unblocked (!want_blocked) in the calling thread.
int CheckMask(bool want_blocked) {
  sigset_t current;
  pthread_sigmask(SIG_BLOCK, nullptr, &current);
  for (const int sig : kInitSignals) {
    if ((sigismember(&current, sig) == 1) != want_blocked) return 10 + sig;
  }
  return 0;
}

// Runs in a forked child so the test process keeps its own mask.
TEST(Pid1Test, InitSignalsBlockedAndUnblockedInForkChildren) {
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    pid1::BlockInitSignals();
    if (const int rc = CheckMask(true); rc != 0) ::_exit(rc);

    // A service forked from init must get the signals back (pthread_atfork).
    const pid_t grandchild = ::fork();
    if (grandchild == 0) ::_exit(CheckMask(false));
    int status = 0;
    ::waitpid(grandchild, &status, 0);
    ::_exit(WIFEXITED(status) ? 100 + WEXITSTATUS(status) : 99);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  // 100 + 0: blocked in "init", unblocked in its fork child.
  EXPECT_EQ(WEXITSTATUS(status), 100);
}

// A blocked SIGTERM must not kill the process (init takes it via signalfd);
// before BlockInitSignals ran first, a thread with it unblocked could take
// the default action and kill init.
TEST(Pid1Test, BlockedSigtermDoesNotKill) {
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    pid1::BlockInitSignals();
    ::kill(::getpid(), SIGTERM);
    ::usleep(100'000);
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
