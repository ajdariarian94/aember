/**
 * @file child-supervisor.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Child process supervision and bookkeeping.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <sys/types.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aember::child_supervisor {

/**
 * ChildSupervisor tracks child processes spawned by the init system.
 *
 * Responsibilities:
 *  - bookkeeping of pid → name entries
 *  - reaping exited children via waitpid on SIGCHLD
 *  - diagnostic logging of child lifecycle events
 *
 * It does not own restart logic — that belongs to ProcessManager.
 */
class ChildSupervisor {
 public:
  using Logger = aember::utils::logging::Logger;

  ChildSupervisor();

  ChildSupervisor(const ChildSupervisor&) = delete;
  ChildSupervisor& operator=(const ChildSupervisor&) = delete;

  // ---------------------------------------------------------------------------
  // Child tracking
  // ---------------------------------------------------------------------------

  /** Register a newly spawned child. Call immediately after fork/exec. */
  void AddChild(pid_t pid, std::string_view name);

  /** Explicitly deregister a child (most are removed automatically on reap). */
  void RemoveChild(pid_t pid);

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /** Call from the SIGCHLD handler — delegates to ReapChildren(). */
  void HandleSIGCHLD();

  /** Non-blocking waitpid loop — reaps all exited children. */
  void ReapChildren();

  // ---------------------------------------------------------------------------
  // Shutdown
  // ---------------------------------------------------------------------------

  /** Log and clear all tracked children during shutdown. */
  void StopAll();

 private:
  /// pid → service name. ChildInfo collapsed — the name is all we need.
  std::unordered_map<pid_t, std::string> children_;

  std::mutex children_mutex_;

  mutable Logger log_{"child-supervisor"};
};

}  // namespace aember::child_supervisor
