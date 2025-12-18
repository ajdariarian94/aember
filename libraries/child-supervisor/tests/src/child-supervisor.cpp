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

/**TEST_F(ChildSupervisorTest, HandleSIGCHLDWithExitedChild) {
  pid_t pid = SpawnQuickExitChild(0);

  supervisor_->AddChild(pid, "quick-exit-child");

  // Wait for child to become a zombie
  ASSERT_TRUE(WaitForZombie(pid, std::chrono::seconds(2)));

  // Verify it's a zombie before reaping
  EXPECT_TRUE(IsZombie(pid));

  // Handle SIGCHLD (which should reap the child)
  supervisor_->HandleSIGCHLD();

  // Give it a moment to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify the process has been reaped
  EXPECT_FALSE(IsZombie(pid));
}**/

TEST_F(ChildSupervisorTest, HandleSIGCHLDWithMultipleExitedChildren) {
  pid_t pid1 = SpawnQuickExitChild(0);
  pid_t pid2 = SpawnQuickExitChild(0);
  pid_t pid3 = SpawnQuickExitChild(0);

  supervisor_->AddChild(pid1, "child-1");
  supervisor_->AddChild(pid2, "child-2");
  supervisor_->AddChild(pid3, "child-3");

  // Wait for all children to become zombies
  ASSERT_TRUE(WaitForZombie(pid1, std::chrono::seconds(2)));
  ASSERT_TRUE(WaitForZombie(pid2, std::chrono::seconds(2)));
  ASSERT_TRUE(WaitForZombie(pid3, std::chrono::seconds(2)));

  // Handle SIGCHLD (should reap all children)
  supervisor_->HandleSIGCHLD();

  // Give it a moment to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify all have been reaped
  EXPECT_FALSE(IsZombie(pid1));
  EXPECT_FALSE(IsZombie(pid2));
  EXPECT_FALSE(IsZombie(pid3));
}

TEST_F(ChildSupervisorTest, ReapChildrenWithDifferentExitCodes) {
  pid_t pid1 = SpawnQuickExitChild(0);
  pid_t pid2 = SpawnQuickExitChild(1);
  pid_t pid3 = SpawnQuickExitChild(42);

  supervisor_->AddChild(pid1, "success-child");
  supervisor_->AddChild(pid2, "failure-child");
  supervisor_->AddChild(pid3, "custom-exit-child");

  // Wait for children to become zombies
  ASSERT_TRUE(WaitForZombie(pid1, std::chrono::seconds(2)));
  ASSERT_TRUE(WaitForZombie(pid2, std::chrono::seconds(2)));
  ASSERT_TRUE(WaitForZombie(pid3, std::chrono::seconds(2)));

  // Reap all children
  supervisor_->ReapChildren();

  // Give it a moment
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify all have been reaped
  EXPECT_FALSE(IsZombie(pid1));
  EXPECT_FALSE(IsZombie(pid2));
  EXPECT_FALSE(IsZombie(pid3));
}

TEST_F(ChildSupervisorTest, ReapChildrenWithSignaledChild) {
  pid_t pid = SpawnSleepingChild(10);

  supervisor_->AddChild(pid, "signaled-child");

  // Kill the child with a signal
  KillChild(pid, SIGKILL);

  // Wait for child to become a zombie
  ASSERT_TRUE(WaitForZombie(pid, std::chrono::seconds(2)));

  // Reap the child
  supervisor_->ReapChildren();

  // Give it a moment
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify it has been reaped
  EXPECT_FALSE(IsZombie(pid));
}

TEST_F(ChildSupervisorTest, ReapChildrenWithNoChildren) {
  // Should not crash when there are no children to reap
  supervisor_->ReapChildren();
}

TEST_F(ChildSupervisorTest, ReapChildrenWithUnknownChild) {
  // Spawn a child but don't add it to the supervisor
  pid_t pid = SpawnQuickExitChild(0);

  // Wait for it to become a zombie
  ASSERT_TRUE(WaitForZombie(pid, std::chrono::seconds(2)));

  // Reap should still work (it will log it as unknown)
  supervisor_->ReapChildren();

  // Give it a moment
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify it has been reaped
  EXPECT_FALSE(IsZombie(pid));
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

TEST_F(ChildSupervisorTest, ReapAfterRemove) {
  pid_t pid = SpawnQuickExitChild(0);

  supervisor_->AddChild(pid, "test-child");

  // Wait for child to become a zombie
  ASSERT_TRUE(WaitForZombie(pid, std::chrono::seconds(2)));

  // Remove before reaping
  supervisor_->RemoveChild(pid);

  // Reap should still work (won't find it in the map)
  supervisor_->ReapChildren();

  // Give it a moment
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify it has been reaped
  EXPECT_FALSE(IsZombie(pid));
}

TEST_F(ChildSupervisorTest, MultipleReapCalls) {
  pid_t pid = SpawnQuickExitChild(0);

  supervisor_->AddChild(pid, "test-child");

  // Wait for child to become a zombie
  ASSERT_TRUE(WaitForZombie(pid, std::chrono::seconds(2)));

  // Multiple reap calls should be safe
  supervisor_->ReapChildren();
  supervisor_->ReapChildren();
  supervisor_->ReapChildren();

  // Give it a moment
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Verify it has been reaped
  EXPECT_FALSE(IsZombie(pid));
}

TEST_F(ChildSupervisorTest, HandleSIGCHLDWithNoExitedChildren) {
  pid_t pid = SpawnSleepingChild(1);

  supervisor_->AddChild(pid, "sleeping-child");

  // Call HandleSIGCHLD when child is still running
  supervisor_->HandleSIGCHLD();

  // Child should still be running
  EXPECT_EQ(kill(pid, 0), 0);

  // Clean up
  KillChild(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

}  // namespace aember_test::child_supervisor
