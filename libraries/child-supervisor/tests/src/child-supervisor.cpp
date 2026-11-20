#include <aember-libs-tests/child-supervisor/child-supervisor.h>

namespace aember_test::child_supervisor {

TEST_F(ChildSupervisorTest, ConstructorInitializes) {
  // Should not crash
  EXPECT_NE(supervisor_, nullptr);
}

TEST_F(ChildSupervisorTest, AddChildBasic) {
  pid_t pid = SpawnSleepingChild(1);

  supervisor_->AddChild(pid, "test-child");

  // Clean up
  KillChild(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

TEST_F(ChildSupervisorTest, AddMultipleChildren) {
  pid_t pid1 = SpawnSleepingChild(1);
  pid_t pid2 = SpawnSleepingChild(1);
  pid_t pid3 = SpawnSleepingChild(1);

  supervisor_->AddChild(pid1, "child-1");
  supervisor_->AddChild(pid2, "child-2");
  supervisor_->AddChild(pid3, "child-3");

  // Clean up
  KillChild(pid1, SIGKILL);
  KillChild(pid2, SIGKILL);
  KillChild(pid3, SIGKILL);
  waitpid(pid1, nullptr, 0);
  waitpid(pid2, nullptr, 0);
  waitpid(pid3, nullptr, 0);
}

TEST_F(ChildSupervisorTest, RemoveChildBasic) {
  pid_t pid = SpawnSleepingChild(1);

  supervisor_->AddChild(pid, "test-child");
  supervisor_->RemoveChild(pid);

  // Clean up
  KillChild(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

TEST_F(ChildSupervisorTest, RemoveNonExistentChild) {
  // Should not crash when removing a child that was never added
  supervisor_->RemoveChild(99999);
}

TEST_F(ChildSupervisorTest, StopAllWithNoChildren) {
  // Should not crash when stopping with no children
  supervisor_->StopAll();
}

TEST_F(ChildSupervisorTest, StopAllWithRunningChildren) {
  pid_t pid1 = SpawnSleepingChild(10);
  pid_t pid2 = SpawnSleepingChild(10);

  supervisor_->AddChild(pid1, "child-1");
  supervisor_->AddChild(pid2, "child-2");

  // Stop all (should clear internal tracking)
  supervisor_->StopAll();

  // Clean up processes manually (they're still running)
  KillChild(pid1, SIGKILL);
  KillChild(pid2, SIGKILL);
  waitpid(pid1, nullptr, 0);
  waitpid(pid2, nullptr, 0);
}

TEST_F(ChildSupervisorTest, AddRemoveAddSameChild) {
  pid_t pid = SpawnSleepingChild(1);

  supervisor_->AddChild(pid, "test-child");
  supervisor_->RemoveChild(pid);
  supervisor_->AddChild(pid, "test-child-again");

  // Clean up
  KillChild(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

TEST_F(ChildSupervisorTest, ConcurrentAddRemove) {
  std::vector<pid_t> pids;

  // Add multiple children
  for (int i = 0; i < 10; ++i) {
    pid_t pid = SpawnSleepingChild(1);
    pids.push_back(pid);
    supervisor_->AddChild(pid, "child-" + std::to_string(i));
  }

  // Remove some
  for (int i = 0; i < 5; ++i) { supervisor_->RemoveChild(pids[i]); }

  // Clean up
  for (pid_t pid : pids) {
    KillChild(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
}

// ---------------------------------------------------------------------------
// ReapZombies — the PID 1 reaper
// ---------------------------------------------------------------------------

namespace {

// Unlike the fixture's WaitForZombie(), this only reads /proc and never
// reaps the child itself.
bool WaitUntilZombie(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto zombies =
        aember::child_supervisor::ChildSupervisor::ListZombieChildren();
    if (std::ranges::find(zombies, pid) != zombies.end()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

TEST_F(ChildSupervisorTest, ListZombieChildrenFindsOnlyZombies) {
  const pid_t zombie = SpawnQuickExitChild(0);
  const pid_t running = SpawnSleepingChild(5);
  ASSERT_TRUE(WaitUntilZombie(zombie, std::chrono::seconds(2)));

  const auto zombies =
      aember::child_supervisor::ChildSupervisor::ListZombieChildren();
  EXPECT_NE(std::ranges::find(zombies, zombie), zombies.end());
  EXPECT_EQ(std::ranges::find(zombies, running), zombies.end());
}

TEST_F(ChildSupervisorTest, ReapZombiesReapsTrackedAtOnce) {
  const pid_t pid = SpawnQuickExitChild(3);
  supervisor_->AddChild(pid, "tracked");
  ASSERT_TRUE(WaitUntilZombie(pid, std::chrono::seconds(2)));

  const auto exits = supervisor_->ReapZombies(std::chrono::hours{1});

  ASSERT_EQ(exits.size(), 1u);
  EXPECT_EQ(exits[0].pid, pid);
  EXPECT_TRUE(exits[0].tracked);
  EXPECT_EQ(exits[0].name, "tracked");
  EXPECT_TRUE(WIFEXITED(exits[0].status));
  EXPECT_EQ(WEXITSTATUS(exits[0].status), 3);
  EXPECT_FALSE(IsZombie(pid));
  EXPECT_FALSE(supervisor_->IsTracked(pid));
}

TEST_F(ChildSupervisorTest, ReapZombiesReportsSignaledChild) {
  const pid_t pid = SpawnSleepingChild(30);
  supervisor_->AddChild(pid, "killed");
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  ASSERT_TRUE(WaitUntilZombie(pid, std::chrono::seconds(2)));
  const auto exits = supervisor_->ReapZombies(std::chrono::hours{1});
  ASSERT_EQ(exits.size(), 1u);
  EXPECT_TRUE(WIFSIGNALED(exits[0].status));
  EXPECT_EQ(WTERMSIG(exits[0].status), SIGKILL);
}

TEST_F(ChildSupervisorTest, ReapZombiesWaitsGraceForUntracked) {
  const pid_t pid = SpawnQuickExitChild(0);
  ASSERT_TRUE(WaitUntilZombie(pid, std::chrono::seconds(2)));

  const auto grace = std::chrono::milliseconds{300};

  // First sight: within the grace period, left for its own waiter.
  EXPECT_TRUE(supervisor_->ReapZombies(grace).empty());
  EXPECT_TRUE(IsZombie(pid));

  std::this_thread::sleep_for(grace + std::chrono::milliseconds{50});

  // Still a zombie after the grace period: an orphan, reaped.
  const auto exits = supervisor_->ReapZombies(grace);
  ASSERT_EQ(exits.size(), 1u);
  EXPECT_EQ(exits[0].pid, pid);
  EXPECT_FALSE(exits[0].tracked);
  EXPECT_FALSE(IsZombie(pid));
}

// The bug the reaper fixes: a blind waitpid(-1) in PID 1 stole the exit
// status of children that other code waits for itself (system(), modprobe,
// liblxc), so their waitpid(pid) failed with ECHILD.
TEST_F(ChildSupervisorTest, ReapZombiesDoesNotStealFromSynchronousWaiter) {
  std::atomic<bool> stop{false};
  std::thread reaper{[&] {
    while (!stop) {
      supervisor_->ReapZombies(std::chrono::milliseconds{500});
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }};

  int failures = 0;
  for (int i = 0; i < 50; ++i) {
    const pid_t pid = fork();
    if (pid == 0) { _exit(7); }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 7) {
      ++failures;
    }
  }

  stop = true;
  reaper.join();
  EXPECT_EQ(failures, 0);
}

// Regression: a tracked child collected by someone else (ProcessManager::Stop
// waiting for its own child first) stayed tracked forever.
TEST_F(ChildSupervisorTest, ForgetsTrackedChildReapedElsewhere) {
  const pid_t pid = ::fork();
  if (pid == 0) ::_exit(0);
  supervisor_->AddChild(pid, "collected-elsewhere");
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);  // not the supervisor
  EXPECT_TRUE(supervisor_->ReapZombies().empty());
  EXPECT_FALSE(supervisor_->IsTracked(pid));
}

TEST_F(ChildSupervisorTest, KeepsTrackingARunningChild) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::pause();
    ::_exit(0);
  }
  supervisor_->AddChild(pid, "running");
  EXPECT_TRUE(supervisor_->ReapZombies().empty());
  EXPECT_TRUE(supervisor_->IsTracked(pid));
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
}

}  // namespace aember_test::child_supervisor
