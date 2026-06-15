/**
 * @file child-supervisor.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of ChildSupervisor.
 */

#include <aember-libs/child-supervisor/child-supervisor.h>

#include <errno.h>
#include <sys/wait.h>

namespace aember::child_supervisor {

// ---------------------------------------------------------------------------
// Ctor
// ---------------------------------------------------------------------------

ChildSupervisor::ChildSupervisor() {
  log_.info("ChildSupervisor initialized");
}

// ---------------------------------------------------------------------------
// Child tracking
// ---------------------------------------------------------------------------

void ChildSupervisor::AddChild(pid_t pid, std::string_view name) {
  std::lock_guard lock{children_mutex_};
  children_.emplace(pid, std::string{name});
  log_.info("Tracking child '{}' (pid={})", name, pid);
}

void ChildSupervisor::RemoveChild(pid_t pid) {
  std::lock_guard lock{children_mutex_};

  auto it = children_.find(pid);
  if (it == children_.end()) {
    log_.warn("RemoveChild: unknown pid={}", pid);
    return;
  }

  log_.info("Removed child '{}' (pid={})", it->second, pid);
  children_.erase(it);
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

void ChildSupervisor::HandleSIGCHLD() {
  log_.debug("SIGCHLD received");
  ReapChildren();
}

void ChildSupervisor::ReapChildren() {
  // Interpret waitpid status into a log-friendly string.
  const auto describe_exit = [](int status) -> std::string {
    if (WIFEXITED(status)) {
      return std::format("exited with code {}", WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
      return std::format("killed by signal {}", WTERMSIG(status));
    }
    return "exited with unknown status";
  };

  while (true) {
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, WNOHANG);

    if (pid == 0)  { break; }  // no more exited children

    if (pid < 0) {
      if (errno != ECHILD) {
        log_.error("waitpid failed: errno={}", errno);
      }
      break;
    }

    // Look up name and remove from tracking.
    std::string name = "<unknown>";
    {
      std::lock_guard lock{children_mutex_};
      if (auto it = children_.find(pid); it != children_.end()) {
        name = std::move(it->second);
        children_.erase(it);
      }
    }

    log_.info("Child '{}' (pid={}) {}", name, pid, describe_exit(status));
  }
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ChildSupervisor::StopAll() {
  std::lock_guard lock{children_mutex_};

  if (!children_.empty()) {
    log_.warn("StopAll: {} child(ren) still tracked at shutdown",
              children_.size());

    for (const auto& [pid, name] : children_) {
      log_.warn("  '{}' (pid={})", name, pid);
    }
  }

  children_.clear();
  log_.info("ChildSupervisor stopped");
}

}  // namespace aember::child_supervisor