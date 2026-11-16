/**
 * @file child-supervisor.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Child process supervision and bookkeeping.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

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
 *
 * Reaping in PID 1: ReapZombies() is the only place that should collect
 * children. It never calls waitpid(-1): other code in the same process
 * (system(), modprobe, liblxc, the debug shell) forks and waits for its own
 * child with waitpid(pid), and a blind waitpid(-1) steals that status (the
 * caller then sees ECHILD). Instead it lists our zombie children in /proc and
 * reaps tracked ones at once and untracked ones only after a grace period:
 * a synchronous waiter collects its child immediately, so a zombie that is
 * still there after the grace period is an orphan nobody will wait for.
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

  /** Non-blocking waitpid(-1) loop — reaps all exited children.
   *  Only safe when nothing else in the process waits for its own children;
   *  PID 1 uses ReapZombies(). */
  void ReapChildren();

  /** One collected child. */
  struct Exit {
    pid_t pid;
    int status;    ///< raw waitpid status
    bool tracked;  ///< registered with AddChild
    std::string name;
  };

  /** Reap zombie children (see class comment). Tracked children are reaped
   *  at once, untracked ones after they stayed zombies for @p grace.
   *  @p proc_root is the procfs mount (tests may pass another path). */
  std::vector<Exit> ReapZombies(
      std::chrono::milliseconds grace = std::chrono::seconds{2},
      const std::filesystem::path& proc_root = "/proc");

  /** True if @p pid was registered with AddChild and not yet reaped. */
  bool IsTracked(pid_t pid);

  /** pids of our children that are zombies right now (from proc_root). */
  static std::vector<pid_t> ListZombieChildren(
      const std::filesystem::path& proc_root = "/proc");

  // ---------------------------------------------------------------------------
  // Shutdown
  // ---------------------------------------------------------------------------

  /** Log and clear all tracked children during shutdown. */
  void StopAll();

 private:
  /// pid → service name. ChildInfo collapsed — the name is all we need.
  std::unordered_map<pid_t, std::string> children_;

  std::mutex children_mutex_;

  /// Untracked zombies seen by ReapZombies() and when they were first seen.
  std::unordered_map<pid_t, std::chrono::steady_clock::time_point>
      untracked_seen_;

  mutable Logger log_{"child-supervisor"};
};

}  // namespace aember::child_supervisor
