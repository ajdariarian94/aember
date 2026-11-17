/**
 * @file service-manager.cpp
 * @author Arian Ajdari
 * @brief ServiceManager and DependencyResolver tests with real processes
 *        (sleep) and the real Process/Container managers (no containers:
 *        those need LXC and root).
 * @version 0.2
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/container-manager/container-manager.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/process-manager/process-manager.h>
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/service-manager/service-manager.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>

namespace {

using aember::service_manager::DependencyResolver;
using aember::service_manager::ServiceManager;
using ProcessManager = aember::process_manager::ProcessManager;
using State = ProcessManager::ProcessState;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// DependencyResolver
// ---------------------------------------------------------------------------

std::size_t IndexOf(const std::vector<std::string>& v, const std::string& s) {
  return static_cast<std::size_t>(std::ranges::find(v, s) - v.begin());
}

TEST(DependencyResolverTest, DependenciesComeFirst) {
  DependencyResolver r{[](const std::string&) { return true; }};
  const auto order = r.ResolveStartOrder({
      {"app", {"db", "net"}},
      {"db", {"net"}},
      {"net", {}},
      {"log", {}},
  });
  ASSERT_EQ(order.size(), 4u);
  EXPECT_LT(IndexOf(order, "net"), IndexOf(order, "db"));
  EXPECT_LT(IndexOf(order, "db"), IndexOf(order, "app"));
  EXPECT_LT(IndexOf(order, "log"), order.size());
}

TEST(DependencyResolverTest, EmptyGraph) {
  DependencyResolver r{[](const std::string&) { return true; }};
  EXPECT_TRUE(r.ResolveStartOrder({}).empty());
}

// Regression: a cycle used to throw, and StartAll then started nothing.
TEST(DependencyResolverTest, CycleSkipsOnlyTheCycle) {
  DependencyResolver r{[](const std::string&) { return true; }};
  std::vector<std::string> order;
  EXPECT_NO_THROW(order = r.ResolveStartOrder({
                      {"a", {"b"}},
                      {"b", {"a"}},
                      {"ok", {}},
                      {"after-ok", {"ok"}},
                  }));
  EXPECT_EQ(order, (std::vector<std::string>{"ok", "after-ok"}));
}

TEST(DependencyResolverTest, UnknownDependencySkipsOnlyThatEntry) {
  DependencyResolver r{[](const std::string&) { return true; }};
  const auto order = r.ResolveStartOrder({
      {"needs-ghost", {"ghost"}},
      {"fine", {}},
  });
  EXPECT_EQ(order, (std::vector<std::string>{"fine"}));
}

TEST(DependencyResolverTest, CheckDependencies) {
  std::set<std::string> running{"net"};
  DependencyResolver r{
      [&](const std::string& n) { return running.contains(n); }};
  const DependencyResolver::DependencyGraph graph{
      {"app", {"net", "db"}}, {"net", {}}, {"db", {}}, {"bad", {"ghost"}}};

  EXPECT_TRUE(r.CheckDependencies("net", graph));
  EXPECT_FALSE(r.CheckDependencies("app", graph));  // db not running
  running.insert("db");
  EXPECT_TRUE(r.CheckDependencies("app", graph));
  EXPECT_FALSE(r.CheckDependencies("bad", graph));      // unknown dependency
  EXPECT_FALSE(r.CheckDependencies("missing", graph));  // not in graph
}

// ---------------------------------------------------------------------------
// ServiceManager
// ---------------------------------------------------------------------------

class ServiceManagerTest : public ::testing::Test {
 protected:
  ServiceManagerTest()
      : mounts_(std::make_shared<aember::mount_manager::MountManager>()),
        containers_(mounts_),
        resolver_([this](const std::string& name) {
          return sm_ && sm_->GetState(name) == State::Running;
        }) {
    sm_ = std::make_unique<ServiceManager>(
        processes_, containers_, resolver_, supervisor_);
    sm_->SetStateChangeCallback(
        [this](const std::string& name, State, State to) {
          std::lock_guard lock{mutex_};
          transitions_.emplace_back(name, to);
        });
  }

  ~ServiceManagerTest() override { sm_.reset(); }

  /// Register a `sleep 30` process in both managers.
  void AddSleeper(const std::string& name, std::vector<std::string> deps = {}) {
    ProcessManager::ProcessConfig cfg;
    cfg.name = name;
    cfg.executable = "sleep";
    cfg.args = {"30"};
    cfg.dependencies = std::move(deps);
    cfg.restart_on_failure = false;
    cfg.stop_timeout = 2s;
    ASSERT_TRUE(processes_.AddProcess(cfg));
    ASSERT_TRUE(sm_->AddProcess(name));
  }

  /// Names in the order they reached Running.
  std::vector<std::string> RunningOrder() {
    std::lock_guard lock{mutex_};
    std::vector<std::string> names;
    for (const auto& [name, to] : transitions_) {
      if (to == State::Running) names.push_back(name);
    }
    return names;
  }

  std::vector<State> TransitionsOf(const std::string& name) {
    std::lock_guard lock{mutex_};
    std::vector<State> states;
    for (const auto& [n, to] : transitions_) {
      if (n == name) states.push_back(to);
    }
    return states;
  }

  ProcessManager processes_;
  std::shared_ptr<aember::mount_manager::MountManager> mounts_;
  aember::container_manager::ContainerManager containers_;
  aember::child_supervisor::ChildSupervisor supervisor_;
  DependencyResolver resolver_;
  std::unique_ptr<ServiceManager> sm_;

  std::mutex mutex_;
  std::vector<std::pair<std::string, State>> transitions_;
};

TEST_F(ServiceManagerTest, AddRequiresRegistrationInTheManagers) {
  EXPECT_FALSE(sm_->AddProcess("unknown"));
  EXPECT_FALSE(sm_->AddContainer("unknown"));
  EXPECT_FALSE(sm_->Has("unknown"));
  AddSleeper("known");
  EXPECT_TRUE(sm_->Has("known"));
  EXPECT_FALSE(sm_->IsContainer("known"));
  EXPECT_EQ(sm_->GetState("known"), State::Stopped);
}

TEST_F(ServiceManagerTest, StartAndStopAProcess) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  EXPECT_EQ(sm_->GetState("s"), State::Running);
  const auto pid = processes_.GetPid("s");
  ASSERT_TRUE(pid.has_value());
  EXPECT_TRUE(supervisor_.IsTracked(*pid));
  EXPECT_EQ(::kill(*pid, 0), 0);

  // Starting again is a no-op.
  EXPECT_TRUE(sm_->Start("s"));
  EXPECT_EQ(processes_.GetPid("s"), pid);

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(sm_->Stop("s"));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1500ms);
  EXPECT_EQ(sm_->GetState("s"), State::Stopped);
  EXPECT_FALSE(processes_.GetPid("s").has_value());
}

TEST_F(ServiceManagerTest, StateCallbackSeesEveryTransition) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  ASSERT_TRUE(sm_->Stop("s"));
  const auto states = TransitionsOf("s");
  ASSERT_GE(states.size(), 4u);
  EXPECT_EQ(states[0], State::Starting);
  EXPECT_EQ(states[1], State::Running);
  EXPECT_EQ(states[2], State::Stopping);
  EXPECT_EQ(states.back(), State::Stopped);
}

TEST_F(ServiceManagerTest, UnknownNames) {
  EXPECT_FALSE(sm_->Start("ghost"));
  EXPECT_FALSE(sm_->Stop("ghost"));
  EXPECT_FALSE(sm_->Remove("ghost"));
  EXPECT_EQ(sm_->GetState("ghost"), State::Failed);
}

TEST_F(ServiceManagerTest, StartAllFollowsDependencies) {
  AddSleeper("app", {"db"});
  AddSleeper("db", {"net"});
  AddSleeper("net");
  sm_->StartAll();
  EXPECT_EQ(RunningOrder(), (std::vector<std::string>{"net", "db", "app"}));
  for (const auto* n : {"net", "db", "app"}) {
    EXPECT_EQ(sm_->GetState(n), State::Running) << n;
  }
}

// Regression: a broken dependency used to make StartAll start nothing.
TEST_F(ServiceManagerTest, StartAllStartsEverythingNotBroken) {
  AddSleeper("good");
  AddSleeper("cycle-a", {"cycle-b"});
  AddSleeper("cycle-b", {"cycle-a"});
  AddSleeper("orphan", {"ghost"});
  EXPECT_NO_THROW(sm_->StartAll());
  EXPECT_EQ(sm_->GetState("good"), State::Running);
  EXPECT_NE(sm_->GetState("cycle-a"), State::Running);
  EXPECT_NE(sm_->GetState("orphan"), State::Running);
}

TEST_F(ServiceManagerTest, StartRefusesWhileADependencyIsDown) {
  AddSleeper("net");
  AddSleeper("app", {"net"});
  EXPECT_FALSE(sm_->Start("app"));
  EXPECT_NE(sm_->GetState("app"), State::Running);
  ASSERT_TRUE(sm_->Start("net"));
  EXPECT_TRUE(sm_->Start("app"));
}

TEST_F(ServiceManagerTest, CrashIsReportedThroughHandleExit) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  const pid_t pid = *processes_.GetPid("s");
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);

  sm_->HandleExit(pid, status);
  EXPECT_EQ(sm_->GetState("s"), State::Failed);
  EXPECT_FALSE(processes_.GetPid("s").has_value());
}

TEST_F(ServiceManagerTest, HandleExitOfAStrangerIsIgnored) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  EXPECT_NO_THROW(sm_->HandleExit(999999, 0));
  EXPECT_EQ(sm_->GetState("s"), State::Running);
}

TEST_F(ServiceManagerTest, RestartGivesANewProcess) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  const pid_t before = *processes_.GetPid("s");
  ASSERT_TRUE(sm_->Restart("s"));
  const auto after = processes_.GetPid("s");
  ASSERT_TRUE(after.has_value());
  EXPECT_NE(*after, before);
  EXPECT_EQ(sm_->GetState("s"), State::Running);
}

TEST_F(ServiceManagerTest, RemoveStopsAndForgets) {
  AddSleeper("s");
  ASSERT_TRUE(sm_->Start("s"));
  const pid_t pid = *processes_.GetPid("s");
  EXPECT_TRUE(sm_->Remove("s"));
  EXPECT_FALSE(sm_->Has("s"));
  EXPECT_FALSE(processes_.HasProcess("s"));
  // The process is gone (Stop collected it).
  EXPECT_NE(::kill(pid, 0), 0);
}

TEST_F(ServiceManagerTest, StopAllStopsEverything) {
  AddSleeper("a");
  AddSleeper("b");
  AddSleeper("c");
  sm_->StartAll();
  sm_->StopAll();
  for (const auto& name : sm_->GetNames()) {
    EXPECT_EQ(sm_->GetState(name), State::Stopped) << name;
  }
  EXPECT_EQ(sm_->GetNames().size(), 3u);
}

}  // namespace
