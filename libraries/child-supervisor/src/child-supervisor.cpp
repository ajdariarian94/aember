#include <aember-libs/child-supervisor/child-supervisor.h>

#include <errno.h>
#include <sys/wait.h>

namespace aember::child_supervisor {

ChildSupervisor::ChildSupervisor() : log_("child-supervisor") {
  log_.info("ChildSupervisor initialized");
}

void ChildSupervisor::AddChild(pid_t pid, const std::string& name) {
  std::lock_guard lock(children_mutex_);

  children_.emplace(pid, ChildInfo{name});

  log_.info("Added child '{}' (pid={})", name, pid);
}

void ChildSupervisor::RemoveChild(pid_t pid) {
  std::lock_guard lock(children_mutex_);

  auto it = children_.find(pid);
  if (it == children_.end()) {
    log_.warn("Attempted to remove unknown child pid={}", pid);
    return;
  }

  log_.info("Removing child '{}' (pid={})", it->second.name, pid);
  children_.erase(it);
}

void ChildSupervisor::HandleSIGCHLD() {
  log_.debug("Handling SIGCHLD");
  ReapChildren();
}

void ChildSupervisor::ReapChildren() {
  while (true) {
    int status = 0;
    pid_t pid = ::waitpid(-1, &status, WNOHANG);

    if (pid == 0) {
      // No more exited children
      break;
    }

    if (pid < 0) {
      if (errno == ECHILD) {
        // No children left
        break;
      }

      log_.error("waitpid failed: errno={}", errno);
      break;
    }

    std::string name = "<unknown>";

    {
      std::lock_guard lock(children_mutex_);
      auto it = children_.find(pid);
      if (it != children_.end()) {
        name = it->second.name;
        children_.erase(it);
      }
    }

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

  for (const auto& [pid, info] : children_) {
    log_.warn("Child '{}' (pid={}) still running at shutdown", info.name, pid);
  }

  children_.clear();
}

}  // namespace aember::child_supervisor
