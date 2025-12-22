/**
 * @file child-supervisor.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of ChildSupervisor.
 *
 * This file contains the implementation responsible for tracking,
 * reaping, and logging child process lifecycle events. It integrates
 * with SIGCHLD handling but does not implement service restart logic.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>

#include <errno.h>
#include <sys/wait.h>

namespace aember::child_supervisor {

ChildSupervisor::ChildSupervisor() : log_("child-supervisor") {
  log_.info("ChildSupervisor initialized");
}

void ChildSupervisor::AddChild(pid_t pid, const std::string& name) {
  // Protect access to children_ map
  std::lock_guard lock(children_mutex_);

  // Track the new child process
  children_.emplace(pid, ChildInfo{name});

  log_.info("Added child '{}' (pid={})", name, pid);
}

void ChildSupervisor::RemoveChild(pid_t pid) {
  std::lock_guard lock(children_mutex_);

  auto it = children_.find(pid);
  if (it == children_.end()) {
    // Defensive check: removal of unknown child
    log_.warn("Attempted to remove unknown child pid={}", pid);
    return;
  }

  log_.info("Removing child '{}' (pid={})", it->second.name, pid);
  children_.erase(it);
}

void ChildSupervisor::HandleSIGCHLD() {
  // SIGCHLD handler entry point
  // Actual reaping logic is delegated to ReapChildren()
  log_.debug("Handling SIGCHLD");
  ReapChildren();
}

void ChildSupervisor::ReapChildren() {
  // Reap all exited child processes without blocking
  while (true) {
    int status = 0;
    pid_t pid = ::waitpid(-1, &status, WNOHANG);

    if (pid == 0) {
      // No exited children available
      break;
    }

    if (pid < 0) {
      if (errno == ECHILD) {
        // No child processes remain
        break;
      }

      // Unexpected waitpid failure
      log_.error("waitpid failed: errno={}", errno);
      break;
    }

    // Default name in case the child was not tracked
    std::string name = "<unknown>";

    {
      // Remove child from tracking map if present
      std::lock_guard lock(children_mutex_);
      auto it = children_.find(pid);
      if (it != children_.end()) {
        name = it->second.name;
        children_.erase(it);
      }
    }

    // Log exit details
    if (WIFEXITED(status)) {
      log_.info("Child '{}' (pid={}) exited with code {}",
                name,
                pid,
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      log_.warn("Child '{}' (pid={}) killed by signal {}",
                name,
                pid,
                WTERMSIG(status));
    } else {
      log_.warn("Child '{}' (pid={}) exited with unknown status", name, pid);
    }
  }
}

void ChildSupervisor::StopAll() {
  std::lock_guard lock(children_mutex_);

  log_.info("Stopping ChildSupervisor, {} children still tracked",
            children_.size());

  // Log any children still present at shutdown for diagnostics
  for (const auto& [pid, info] : children_) {
    log_.warn("Child '{}' (pid={}) still running at shutdown", info.name, pid);
  }

  // Clear internal tracking state
  children_.clear();
}

}  // namespace aember::child_supervisor
