/**
 * @file child-supervisor.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Child process supervision and bookkeeping.
 *
 * The ChildSupervisor tracks child processes spawned by the init system.
 * It provides centralized bookkeeping, logging, and cleanup for child
 * processes, and integrates with SIGCHLD handling.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <sys/types.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aember::child_supervisor {

/**
 * @class ChildSupervisor
 * @brief Tracks and supervises child processes.
 *
 * The ChildSupervisor is responsible for:
 * - Tracking child PIDs and associated metadata
 * - Reaping exited children
 * - Providing diagnostic logging for child lifecycle events
 *
 * It does not own service restart logic; that responsibility belongs
 * to the ServiceManager. This class focuses purely on process tracking.
 */
class ChildSupervisor {
 public:
  using Logger = aember::utils::logging::Logger;

  /**
   * @brief Construct a new ChildSupervisor.
   *
   * Initializes internal data structures and logger.
   */
  ChildSupervisor();

  /**
   * @brief Register a newly spawned child process.
   *
   * This should be called immediately after a successful fork/exec
   * to begin tracking the child.
   *
   * @param pid  Process ID of the spawned child
   * @param name Human-readable name of the child (e.g. service name)
   */
  void AddChild(pid_t pid, const std::string& name);

  /**
   * @brief Remove a child from supervision.
   *
   * This can be used to explicitly forget a child process, though in
   * most cases children are removed automatically when reaped.
   *
   * @param pid Process ID of the child to remove
   */
  void RemoveChild(pid_t pid);

  /**
   * @brief Handle SIGCHLD notifications.
   *
   * This function should be called when a SIGCHLD signal is received.
   * It typically performs non-blocking reap operations and logs
   * child exit events.
   */
  void HandleSIGCHLD();

  /**
   * @brief Stop supervising all children.
   *
   * Used during shutdown to clean up internal state and ensure that
   * no stale child entries remain.
   */
  void StopAll();

  /**
   * @brief Reap exited child processes.
   *
   * Performs waitpid-based reaping of child processes and removes them
   * from the internal tracking map.
   */
  void ReapChildren();

 private:
  /**
   * @struct ChildInfo
   * @brief Metadata associated with a supervised child process.
   */
  struct ChildInfo {
    /// Human-readable name associated with the child process
    std::string name;
  };

  /**
   * Map of active child processes indexed by PID.
   *
   * Protected by children_mutex_.
   */
  std::unordered_map<pid_t, ChildInfo> children_;

  /**
   * Mutex protecting access to the children_ map.
   */
  std::mutex children_mutex_;

  /**
   * Logger instance for child supervision events.
   */
  Logger log_;
};

}  // namespace aember::child_supervisor
