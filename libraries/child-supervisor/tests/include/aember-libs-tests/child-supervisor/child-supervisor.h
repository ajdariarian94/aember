#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember_test::child_supervisor {

class ChildSupervisorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    supervisor_ = std::make_unique<aember::child_supervisor::ChildSupervisor>();
    spawned_pids_.clear();
  }

  void TearDown() override {
    // Clean up any remaining child processes
    for (pid_t pid : spawned_pids_) {
      // Try to kill the process if it's still running
      if (kill(pid, 0) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
      } else {
        // Process already dead, reap it
        waitpid(pid, nullptr, WNOHANG);
      }
    }
    spawned_pids_.clear();

    if (supervisor_) {
      supervisor_->StopAll();
      supervisor_.reset();
    }
  }

  // Helper: Spawn a child process that exits immediately
  pid_t SpawnQuickExitChild(int exit_code = 0) {
    pid_t pid = fork();
    if (pid == 0) {
      // Child process
      _exit(exit_code);
    }
    spawned_pids_.push_back(pid);
    return pid;
  }

  // Helper: Spawn a child process that sleeps
  pid_t SpawnSleepingChild(int seconds = 10) {
    pid_t pid = fork();
    if (pid == 0) {
      // Child process
      sleep(seconds);
      _exit(0);
    }
    spawned_pids_.push_back(pid);
    return pid;
  }

  // Helper: Wait for a process to become a zombie (exited but not reaped)
  bool WaitForZombie(pid_t pid, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
      int status;
      pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == pid) {
        // Oops, we reaped it. This shouldn't happen but put it back as zombie
        return true;
      }
      if (result == 0) {
        // Still running, check if it's a zombie by looking at /proc
        std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
        FILE* f = fopen(stat_path.c_str(), "r");
        if (f) {
          char state;
          int dummy;
          if (fscanf(f, "%d %*s %c", &dummy, &state) == 2) {
            fclose(f);
            if (state == 'Z') { return true; }
          } else {
            fclose(f);
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  // Helper: Kill a child process
  void KillChild(pid_t pid, int signal = SIGTERM) { kill(pid, signal); }

  // Helper: Check if a process is a zombie
  bool IsZombie(pid_t pid) {
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    FILE* f = fopen(stat_path.c_str(), "r");
    if (!f) return false;

    char state;
    int dummy;
    bool is_zombie = false;
    if (fscanf(f, "%d %*s %c", &dummy, &state) == 2) {
      is_zombie = (state == 'Z');
    }
    fclose(f);
    return is_zombie;
  }

  std::unique_ptr<aember::child_supervisor::ChildSupervisor> supervisor_;
  std::vector<pid_t> spawned_pids_;
};

}  // namespace aember_test::child_supervisor
