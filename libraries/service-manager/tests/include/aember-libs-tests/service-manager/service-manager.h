#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/service-manager/service-manager.h>

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace aember_test::service_manager {

class ServiceManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    supervisor_ = std::make_unique<aember::child_supervisor::ChildSupervisor>();
    manager_ =
        std::make_unique<aember::service_manager::ServiceManager>(*supervisor_);

    state_changes_.clear();
  }

  void TearDown() override {
    // Just reset - destructor will handle StopAll
    manager_.reset();
    supervisor_.reset();

    // Clean up any remaining zombie processes
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
      // Reap all zombies
    }
  }

  // Helper: Create a basic service config
  aember::service_manager::ServiceConfig CreateTestConfig(
      const std::string& name, const std::string& command) {
    aember::service_manager::ServiceConfig config(name, command);
    return config;
  }

  // Helper: Wait for service to reach a specific state
  bool WaitForServiceState(
      const std::string& name,
      aember::service_manager::ServiceState expected_state,
      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
      if (manager_->GetServiceState(name) == expected_state) { return true; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  // Helper: Track state changes
  void TrackStateChanges() {
    manager_->SetStateChangeCallback(
        [this](const std::string& name,
               aember::service_manager::ServiceState old_state,
               aember::service_manager::ServiceState new_state) {
          state_changes_.push_back({name, old_state, new_state});
        });
  }

  // Add to test fixture header
  void ReapAllRunningServices() {
    std::set<pid_t> pids;

    for (const auto& name : manager_->GetServiceNames()) {
      auto service = manager_->GetService(name);
      if (service && service->GetPid() > 0) { pids.insert(service->GetPid()); }
    }

    // Reap all PIDs
    while (!pids.empty()) {
      int status;
      pid_t reaped = waitpid(-1, &status, WNOHANG);

      if (reaped > 0 && pids.count(reaped)) {
        int exit_code =
            WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        manager_->HandleServiceExit(reaped, exit_code);
        pids.erase(reaped);
      } else if (reaped <= 0) {
        // Try blocking wait on first remaining PID
        if (!pids.empty()) {
          pid_t try_pid = *pids.begin();
          reaped = waitpid(try_pid, &status, 0);
          if (reaped > 0) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                              : 128 + WTERMSIG(status);
            manager_->HandleServiceExit(reaped, exit_code);
            pids.erase(reaped);
          }
        }
      }
    }
  }

  struct StateChange {
    std::string name;
    aember::service_manager::ServiceState old_state;
    aember::service_manager::ServiceState new_state;
  };

  std::unique_ptr<aember::child_supervisor::ChildSupervisor> supervisor_;
  std::unique_ptr<aember::service_manager::ServiceManager> manager_;
  std::vector<StateChange> state_changes_;
};

}  // namespace aember_test::service_manager
