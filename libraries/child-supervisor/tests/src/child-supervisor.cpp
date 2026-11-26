#include <filesystem>
#include <fstream>
#include <string>

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

// PID 1's reaper and containerd scan /proc only after a SIGCHLD, or while
// this says an untracked zombie waits out its grace period.
TEST_F(ChildSupervisorTest, HasUntrackedZombiesDuringTheGracePeriod) {
  using aember::child_supervisor::ChildSupervisor;
  using namespace std::chrono_literals;
  ChildSupervisor supervisor;
  EXPECT_FALSE(supervisor.HasUntrackedZombies());
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) ::_exit(0);  // untracked: never AddChild'ed
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  const auto is_zombie = [pid] {
    const auto zombies = ChildSupervisor::ListZombieChildren();
    return std::ranges::find(zombies, pid) != zombies.end();
  };
  while (!is_zombie() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(is_zombie());
  EXPECT_TRUE(supervisor.ReapZombies(1h).empty());  // within the grace
  EXPECT_TRUE(supervisor.HasUntrackedZombies());
  const auto exits = supervisor.ReapZombies(0ms);
  ASSERT_EQ(exits.size(), 1u);
  EXPECT_EQ(exits[0].pid, pid);
  EXPECT_FALSE(supervisor.HasUntrackedZombies());
}

// A fake /proc with malformed entries: only well-formed stat lines count.
TEST_F(ChildSupervisorTest, ProcParsingSkipsMalformedEntries) {
  namespace fs = std::filesystem;
  using aember::child_supervisor::ChildSupervisor;
  const auto root = fs::temp_directory_path() /
                    ("aember-cs-proc-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto self = std::to_string(::getpid());
  const auto stat = [&](const std::string& dir, const std::string& line) {
    fs::create_directories(root / dir);
    std::ofstream{root / dir / "stat"} << line << "\n";
  };
  stat("10", "10 (ok (z)) Z " + self + " 1 1");  // the one real zombie
  stat("77x", "77 (name) Z " + self + " 1 1");   // not a pid directory
  stat("55", "xZ " + self + " 1 1");             // no ')'
  stat("56", "56 (a)");                          // nothing after the ')'
  stat("57", "57 (a) S " + self);                // not a zombie
  EXPECT_EQ(ChildSupervisor::ListZombieChildren(root), std::vector<pid_t>{10});

  stat("60", "60 (a) S " + self + " 1 1");
  stat("61", "xxx" + self);  // no ')': not ours, even if a number follows
  stat("62", "62 (a) S");    // too short
  // Another parent (not 1: in a PID namespace this test is PID 1).
  stat("63", "63 (a) S " + std::to_string(::getpid() + 1) + " 1 1");
  EXPECT_TRUE(ChildSupervisor::IsChildOfThis(60, root));
  EXPECT_FALSE(ChildSupervisor::IsChildOfThis(61, root));
  EXPECT_FALSE(ChildSupervisor::IsChildOfThis(62, root));
  EXPECT_FALSE(ChildSupervisor::IsChildOfThis(63, root));
  EXPECT_FALSE(ChildSupervisor::IsChildOfThis(64, root));  // gone
  fs::remove_all(root);
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

// Regression: a blind waitpid(-1) in PID 1 stole the exit status of
// children that other code waits for itself (system(), modprobe, liblxc),
// so their waitpid(pid) failed with ECHILD.
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
