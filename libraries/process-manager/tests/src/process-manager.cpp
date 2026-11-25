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
#include <aember-libs/utils/ipc/channel.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

/// Wait until @p pid runs @p program (its exec happened). A signal sent in
/// the window between fork and exec can get lost under valgrind, which then
/// runs its own code in the child.
bool WaitForExec(pid_t pid, const std::string& program) {
  for (int i = 0; i < 200; ++i) {
    std::ifstream comm{"/proc/" + std::to_string(pid) + "/comm"};
    std::string name;
    std::getline(comm, name);
    if (name == program) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
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

// Regression: with PID 1's reaper thread collecting the child first, Stop()
// used to waitpid() a child that was already gone, spin until stop_timeout
// and then SIGKILL a pid that might belong to someone else by then.
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
  ASSERT_TRUE(WaitForExec(*pid, "sleep"));

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(pm.Stop("sleeper"));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);

  stop = true;
  reaper.join();

  EXPECT_FALSE(pm.GetPid("sleeper").has_value());
  EXPECT_EQ(pm.GetState("sleeper"), State::Stopped);
  EXPECT_FALSE(supervisor.IsTracked(*pid));
}

// Regression: a restart pending at shutdown ran on a destroyed manager (the
// manager detached the restart thread). Now the destructor stops it at once
// and the restart never happens.
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

// CancelRestarts (shutdown) stops a pending restart at once, and no new one
// is scheduled until EnableRestarts.
TEST(ProcessManagerTest, CancelRestartsStopsPendingAndNewOnes) {
  std::atomic<int> restarts{0};
  ProcessManager pm;
  pm.SetRestartCallback([&](const std::string&) { ++restarts; });
  ProcessManager::ProcessConfig cfg;
  cfg.name = "quitter";
  cfg.executable = "false";
  cfg.restart_on_failure = true;
  cfg.restart_delay = 50ms;
  ASSERT_TRUE(pm.AddProcess(cfg));
  const auto crash = [&] {
    const auto pid = pm.Start("quitter");
    ASSERT_TRUE(pid.has_value());
    int status = 0;
    ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
    pm.HandleExit(*pid, status);
  };

  crash();  // pending
  pm.CancelRestarts();
  crash();  // not scheduled at all
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(restarts.load(), 0);

  pm.EnableRestarts();
  crash();
  for (int i = 0; i < 100 && restarts.load() == 0; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(restarts.load(), 1);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

TEST(ProcessManagerRegistryTest, RejectsInvalidAndDuplicateConfigs) {
  ProcessManager pm;
  auto cfg = SleepConfig("");
  EXPECT_FALSE(pm.AddProcess(cfg));  // no name
  cfg = SleepConfig("x");
  cfg.executable.clear();
  EXPECT_FALSE(pm.AddProcess(cfg));  // no executable
  EXPECT_TRUE(pm.AddProcess(SleepConfig("x")));
  EXPECT_FALSE(pm.AddProcess(SleepConfig("x")));  // duplicate
  EXPECT_TRUE(pm.HasProcess("x"));
  EXPECT_EQ(pm.GetConfigs().size(), 1u);
  ASSERT_TRUE(pm.GetConfig("x").has_value());
  EXPECT_EQ(pm.GetConfig("x")->args, std::vector<std::string>{"30"});
  EXPECT_FALSE(pm.GetConfig("y").has_value());
}

TEST(ProcessManagerRegistryTest, RemoveStopsAndForgets) {
  ProcessManager pm;
  ASSERT_TRUE(pm.AddProcess(SleepConfig("x")));
  const auto pid = pm.Start("x");
  ASSERT_TRUE(pid);
  EXPECT_TRUE(pm.RemoveProcess("x"));
  EXPECT_FALSE(pm.HasProcess("x"));
  EXPECT_FALSE(Alive(*pid));
  EXPECT_FALSE(pm.RemoveProcess("x"));  // gone already
}

TEST(ProcessManagerRegistryTest, UnknownNames) {
  ProcessManager pm;
  EXPECT_FALSE(pm.Start("nope").has_value());
  EXPECT_EQ(pm.GetState("nope"), State::Failed);
  EXPECT_FALSE(pm.GetPid("nope").has_value());
  EXPECT_FALSE(pm.IsRunning("nope"));
  EXPECT_FALSE(pm.HandleExit(999999, 0));  // not ours
}

TEST(ProcessManagerRegistryTest, LoadProcessesFromFile) {
  ProcessManager pm;
  EXPECT_TRUE(pm.LoadProcesses("/nonexistent/services.json").empty());
  const auto path = std::filesystem::temp_directory_path() /
                    ("pm-test-" + std::to_string(::getpid()) + ".json");
  std::ofstream{path} << R"({"processes":[{"name":"a","executable":"/bin/true"},
                                          {"name":"b","executable":"/bin/true",
                                           "args":["1"]}]})";
  const auto configs = pm.LoadProcesses(path.string());
  std::filesystem::remove(path);
  ASSERT_EQ(configs.size(), 2u);
  EXPECT_EQ(configs[1].args, std::vector<std::string>{"1"});
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(ProcessManagerLifecycleTest, StartingARunningProcessReturnsItsPid) {
  ProcessManager pm;
  ASSERT_TRUE(pm.AddProcess(SleepConfig("x")));
  const auto first = pm.Start("x");
  ASSERT_TRUE(first);
  EXPECT_TRUE(pm.IsRunning("x"));
  EXPECT_EQ(pm.Start("x"), first);  // no second copy
  EXPECT_TRUE(pm.Stop("x"));
}

TEST(ProcessManagerLifecycleTest, StopKillsWhatIgnoresSigterm) {
  ProcessManager pm;
  auto cfg = SleepConfig("stubborn");
  cfg.executable = "/bin/sh";
  cfg.args = {"-c", "trap '' TERM; while :; do sleep 1; done"};
  cfg.stop_timeout = 300ms;
  ASSERT_TRUE(pm.AddProcess(cfg));
  const auto pid = pm.Start("stubborn");
  ASSERT_TRUE(pid);
  std::this_thread::sleep_for(200ms);  // let the trap be set
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(pm.Stop("stubborn"));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 250ms);
  // Regression: the SIGKILL path returned before the child was collected,
  // leaving it Stopping (and a zombie) without a reaper.
  EXPECT_FALSE(pm.GetPid("stubborn").has_value());
  EXPECT_EQ(pm.GetState("stubborn"), State::Stopped);
  EXPECT_FALSE(Alive(*pid));
}

TEST(ProcessManagerLifecycleTest, StoppingSomethingNotRunningIsFine) {
  ProcessManager pm;
  ASSERT_TRUE(pm.AddProcess(SleepConfig("x")));
  EXPECT_TRUE(pm.Stop("x"));
  EXPECT_TRUE(pm.Stop("unknown"));  // nothing to stop
}

TEST(ProcessManagerLifecycleTest, ArgsEnvironmentAndWorkingDirectory) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pm-cwd-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  ProcessManager pm;
  ProcessManager::ProcessConfig cfg;
  cfg.name = "env";
  cfg.executable = "/bin/sh";
  // An absolute output path: a broken chdir (e.g. under `aember mutate`)
  // must fail the check, not drop a file wherever the test runs.
  cfg.args = {"-c", "echo \"$GREETING $1 $(pwd)\" > \"$OUT\"", "sh", "arg1"};
  cfg.environment = {{"GREETING", "hello"}, {"OUT", (dir / "out").string()}};
  cfg.working_directory = dir.string();
  ASSERT_TRUE(pm.AddProcess(cfg));
  const auto pid = pm.Start("env");
  ASSERT_TRUE(pid);
  int status = 0;
  ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
  pm.HandleExit(*pid, status);
  std::ifstream out{dir / "out"};
  std::string line;
  std::getline(out, line);
  std::filesystem::remove_all(dir);
  EXPECT_EQ(line, "hello arg1 " + dir.string());
}

TEST(ProcessManagerLifecycleTest, MissingExecutableExitsWithOne) {
  ProcessManager pm;
  auto cfg = SleepConfig("missing");
  cfg.executable = "/nonexistent/aember-test-binary";
  ASSERT_TRUE(pm.AddProcess(cfg));
  const auto pid = pm.Start("missing");
  ASSERT_TRUE(pid);  // fork worked; the exec in the child didn't
  int status = 0;
  ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 1);
}

TEST(ProcessManagerLifecycleTest, IpcServiceGetsItsChannelOnFd3) {
  ProcessManager pm;
  std::optional<aember::utils::ipc::Channel> ours;
  pm.SetChannelCallback(
      [&](const std::string& name, aember::utils::ipc::Channel channel) {
        EXPECT_EQ(name, "daemon");
        ours = std::move(channel);
      });
  auto cfg = SleepConfig("daemon");
  cfg.executable = "/bin/sh";
  cfg.args = {"-c", R"(printf '{"type":"ready"}' >&3)"};
  cfg.ipc = true;
  ASSERT_TRUE(pm.AddProcess(cfg));
  const auto pid = pm.Start("daemon");
  ASSERT_TRUE(pid);
  ASSERT_TRUE(ours.has_value());
  const auto msg = ours->Receive(5s);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(aember::utils::ipc::TypeOf(*msg), "ready");
  int status = 0;
  ::waitpid(*pid, &status, 0);
}

TEST(ProcessManagerLifecycleTest, DestructorKillsWhatIsStillRunning) {
  pid_t pid = -1;
  {
    ProcessManager pm;
    ASSERT_TRUE(pm.AddProcess(SleepConfig("x")));
    pid = pm.Start("x").value_or(-1);
    ASSERT_GT(pid, 0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

// ---------------------------------------------------------------------------
// Restarts
// ---------------------------------------------------------------------------

TEST(ProcessManagerRestartTest, DisabledRestartLeavesItStopped) {
  ProcessManager pm;
  ASSERT_TRUE(pm.AddProcess(SleepConfig("x")));  // restart_on_failure false
  pm.ScheduleRestart("x");
  EXPECT_NE(pm.GetState("x"), State::Restarting);
  pm.ScheduleRestart("unknown");  // no crash
}

TEST(ProcessManagerRestartTest, GivesUpAfterMaxRestarts) {
  ProcessManager pm;
  std::atomic<int> restarts{0};
  pm.SetRestartCallback([&](const std::string&) { ++restarts; });
  auto cfg = SleepConfig("flaky");
  cfg.restart_on_failure = true;
  cfg.max_restarts = 2;
  cfg.restart_delay = 10ms;
  ASSERT_TRUE(pm.AddProcess(cfg));
  for (int i = 0; i < 2; ++i) {
    pm.ScheduleRestart("flaky");
    EXPECT_EQ(pm.GetState("flaky"), State::Restarting);
  }
  pm.ScheduleRestart("flaky");  // third: over the limit
  EXPECT_EQ(pm.GetState("flaky"), State::Failed);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(restarts.load(), 2);
}

// Found by `aember mutate`: nothing checked how the exit status is recorded.
// Any unrequested exit restarts (restart_on_failure: "on unexpected exit"),
// so the status shows in the state without restarts: exit 0 is Stopped,
// anything else Failed.
TEST(ProcessManagerRestartTest, ExitStatusDecidesStoppedOrFailed) {
  for (const auto& [code, expected] :
       {std::pair{0, State::Stopped}, std::pair{3, State::Failed}}) {
    ProcessManager pm;
    auto cfg = SleepConfig("exiting");  // restart_on_failure false
    cfg.executable = "/bin/sh";
    cfg.args = {"-c", "exit " + std::to_string(code)};
    ASSERT_TRUE(pm.AddProcess(cfg));
    const auto pid = pm.Start("exiting");
    ASSERT_TRUE(pid);
    int status = 0;
    ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
    ASSERT_TRUE(pm.HandleExit(*pid, status));
    EXPECT_EQ(pm.GetState("exiting"), expected) << "exit " << code;
  }
}

TEST(ProcessManagerRestartTest, EveryUnrequestedExitIsRestarted) {
  for (const int code : {0, 3}) {
    ProcessManager pm;
    std::atomic<int> restarts{0};
    pm.SetRestartCallback([&](const std::string&) { ++restarts; });
    auto cfg = SleepConfig("exiting");
    cfg.executable = "/bin/sh";
    cfg.args = {"-c", "exit " + std::to_string(code)};
    cfg.restart_on_failure = true;
    cfg.restart_delay = 10ms;
    ASSERT_TRUE(pm.AddProcess(cfg));
    const auto pid = pm.Start("exiting");
    ASSERT_TRUE(pid);
    int status = 0;
    ASSERT_EQ(::waitpid(*pid, &status, 0), *pid);
    ASSERT_TRUE(pm.HandleExit(*pid, status));
    for (int i = 0; i < 20 && restarts.load() == 0; ++i) {
      std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(restarts.load(), 1) << "exit " << code;
  }
}

// Found by `aember mutate`: nothing checked that a restart waits for its
// restart_delay.
TEST(ProcessManagerRestartTest, RestartWaitsForItsDelay) {
  ProcessManager pm;
  std::atomic<bool> restarted{false};
  pm.SetRestartCallback([&](const std::string&) { restarted = true; });
  auto cfg = SleepConfig("delayed");
  cfg.restart_on_failure = true;
  cfg.restart_delay = 600ms;
  ASSERT_TRUE(pm.AddProcess(cfg));
  pm.ScheduleRestart("delayed");
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(restarted.load()) << "restarted before its delay";
  for (int i = 0; i < 40 && !restarted; ++i) std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(restarted.load());
}

}  // namespace
