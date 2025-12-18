#pragma once

#include <sys/types.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include <aember-libs/utils/logging/logging.h>

namespace aember::child_supervisor {

class ChildSupervisor {
 public:
  ChildSupervisor();

  // Register a newly spawned child
  void AddChild(pid_t pid, const std::string& name);

  // Explicitly remove a child (optional, usually not needed)
  void RemoveChild(pid_t pid);

  // Called when SIGCHLD is received
  void HandleSIGCHLD();

  // Stop supervising (used during shutdown)
  void StopAll();

  void ReapChildren();

 private:
  struct ChildInfo {
    std::string name;
  };

  std::unordered_map<pid_t, ChildInfo> children_;
  std::mutex children_mutex_;

  aember::utils::Logger log_;
};

}  // namespace aember::child_supervisor
