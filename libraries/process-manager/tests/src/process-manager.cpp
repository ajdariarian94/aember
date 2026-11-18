/**
 * @file process-manager.cpp
 * @author Arian Ajdari
 * @brief ProcessManager tests: Stop() with and without the PID 1 reaper.
 * @version 0.1
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/process-manager/process-manager.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include <signal.h>
#include <sys/wait.h>

namespace {

using aember::process_manager::ProcessManager;
using State = ProcessManager::ProcessState;
using namespace std::chrono_literals;

ProcessManager::ProcessConfig SleepConfig(const std::string& name) {
  ProcessManager::ProcessConfig cfg;
  cfg.name = name;
  cfg.executable = "sleep";
  cfg.args = {"30"};
  cfg.restart_on_failure = false;
  cfg.stop_timeout = 5s;
  return cfg;
}

bool Alive(pid_t pid) {
  return kill(pid, 0) == 0;
}

// No reaper running: Stop() collects the child itself and still updates
// the bookkeeping through HandleExit.
TEST(ProcessManagerTest, StopWithoutReaper) {
  std::atomic<int> exits{0};
  ProcessManager pm{[&](const std::string&, int, int) { ++exits; }};
  ASSERT_TRUE(pm.AddProcess(SleepConfig("sleeper")));

  const auto pid = pm.Start("sleeper");
  ASSERT_TRUE(pid.has_value());

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(pm.Stop("sleeper"));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);

  EXPECT_FALSE(pm.GetPid("sleeper").has_value());
  EXPECT_EQ(pm.GetState("sleeper"), State::Stopped);
  EXPECT_EQ(exits.load(), 1);
  EXPECT_FALSE(Alive(*pid));
}

// PID 1 setup: a reaper thread collects the child first. Stop() used to
// waitpid() a child that was already gone, spin until stop_timeout and then
// SIGKILL a pid that might belong to someone else by then.
TEST(ProcessManagerTest, StopWithReaperReturnsPromptly) {
  aember::child_supervisor::ChildSupervisor supervisor;
  ProcessManager pm;
  ASSERT_TRUE(pm.AddProcess(SleepConfig("sleeper")));

  std::atomic<bool> stop{false};
  std::thread reaper{[&] {
    while (!stop) {
      for (const auto& e : supervisor.ReapZombies(10ms)) {
        pm.HandleExit(e.pid, e.status);
      }
      std::this_thread::sleep_for(1ms);
    }
  }};

  const auto pid = pm.Start("sleeper");
  ASSERT_TRUE(pid.has_value());
  supervisor.AddChild(*pid, "sleeper");

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(pm.Stop("sleeper"));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);

  stop = true;
  reaper.join();

  EXPECT_FALSE(pm.GetPid("sleeper").has_value());
  EXPECT_EQ(pm.GetState("sleeper"), State::Stopped);
  EXPECT_FALSE(supervisor.IsTracked(*pid));
}

// A restart pending at shutdown: the manager used to detach the restart
// thread, which then ran on a destroyed manager. Now the destructor stops
// it at once and the restart never happens.
TEST(ProcessManagerTest, DestructorCancelsPendingRestart) {
  std::atomic<int> restarts{0};
  std::optional<ProcessManager> pm{std::in_place};
  pm->SetRestartCallback([&](const std::string&) { ++restarts; });
  ProcessManager::ProcessConfig cfg;
  cfg.name = "quitter";
  cfg.executable = "false";
  cfg.restart_on_failure = true;
  cfg.restart_delay = 30s;
  ASSERT_TRUE(pm->AddProcess(cfg));

  const auto pid = pm->Start("quitter");
  ASSERT_TRUE(pid.has_value());
  int status = 0;
  ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
  pm->HandleExit(*pid, status);  // schedules the restart in 30 s
  EXPECT_EQ(pm->GetState("quitter"), State::Restarting);

  const auto t0 = std::chrono::steady_clock::now();
  pm.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  EXPECT_EQ(restarts.load(), 0);
}

// A due restart still happens (and its thread is cleaned up later).
TEST(ProcessManagerTest, ScheduledRestartRuns) {
  std::atomic<int> restarts{0};
  ProcessManager pm;
  pm.SetRestartCallback([&](const std::string&) { ++restarts; });
  ProcessManager::ProcessConfig cfg;
  cfg.name = "quitter";
  cfg.executable = "false";
  cfg.restart_on_failure = true;
  cfg.restart_delay = 50ms;
  ASSERT_TRUE(pm.AddProcess(cfg));
  for (int round = 1; round <= 2; ++round) {
    const auto pid = pm.Start("quitter");
    ASSERT_TRUE(pid.has_value());
    int status = 0;
    ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
    pm.HandleExit(*pid, status);
    for (int i = 0; i < 100 && restarts.load() < round; ++i) {
      std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(restarts.load(), round);
  }
}

}  // namespace
