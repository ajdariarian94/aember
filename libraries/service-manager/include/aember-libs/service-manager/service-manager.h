/**
 * @file service-manager.h
 * @author Arian Ajdari
 * @brief ServiceManager — PID 1's service supervisor on top of
 *        ProcessManager: names, dependency order, readiness, one state per
 *        service. Containers are not its business any more (they live in
 *        `aember containerd`, which is one of its services).
 * @version 0.6
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/process-manager/process-manager.h>
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/utils/logging/logging.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aember::service_manager {

/**
 * Responsibilities:
 *  - registry of service names (configs live in ProcessManager)
 *  - start in dependency order; a service whose dependencies aren't
 *    running yet waits and starts as soon as they are
 *  - readiness: a service started with `ipc` stays Starting until
 *    MarkReady() (or its ready_timeout, see CheckReadyTimeouts())
 *  - stop in reverse dependency order
 *  - one state per service, reported through StateChangeCallback
 */
class ServiceManager {
 public:
  using ProcessState = aember::process_manager::ProcessManager::ProcessState;
  using Logger = aember::utils::logging::Logger;

  using StateChangeCallback = std::move_only_function<void(
      const std::string& /*name*/, ProcessState /*old_state*/,
      ProcessState /*new_state*/)>;

  ServiceManager(aember::process_manager::ProcessManager& process_manager,
                 DependencyResolver& dependency_resolver,
                 aember::child_supervisor::ChildSupervisor& supervisor);

  ~ServiceManager();

  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  // ---------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------

  /** Config must already be in ProcessManager before calling this. */
  bool AddProcess(std::string_view name);

  /** Deregister. Stops first if running. */
  bool Remove(std::string_view name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  /** Start now if the dependencies are running; otherwise false, and the
   *  service starts as soon as they are (Stop cancels that). */
  bool Start(std::string_view name);
  bool Stop(std::string_view name);
  bool Restart(std::string_view name);

  /** Start everything in dependency order; services whose dependencies
   *  aren't running yet start as soon as they are. */
  void StartAll();
  /** Stop everything, dependents before their dependencies. */
  void StopAll();

  /** The service reported ready over its channel. */
  void MarkReady(std::string_view name);

  /** Whether a service's ready timeout is still pending (boot, a restart):
   *  then CheckReadyTimeouts() should run about every second. */
  bool AwaitingReady() const;

  /** Treat services that didn't report ready within ready_timeout as up
   *  (logged), so their dependents don't wait forever. Call periodically. */
  void CheckReadyTimeouts();

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool Has(std::string_view name) const;
  [[nodiscard]] ProcessState GetState(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> GetNames() const;

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  void HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  void SetStateChangeCallback(StateChangeCallback callback);

 private:
  bool StartInternal(std::string_view name, bool is_restart = false);
  bool StopInternal(std::string_view name);
  /// Set the state and report the change. Caller holds mutex_.
  void SetState(const std::string& name, ProcessState new_state);
  /// Start waiting services whose dependencies are now running.
  void StartWaiting();
  DependencyResolver::DependencyGraph Graph() const;

  aember::process_manager::ProcessManager& process_manager_;
  DependencyResolver& dependency_resolver_;
  aember::child_supervisor::ChildSupervisor& child_supervisor_;

  std::map<std::string, ProcessState> states_;
  /// Started with `ipc`, waiting for "ready": name -> deadline.
  std::map<std::string, std::chrono::steady_clock::time_point> ready_deadline_;
  /// Wanted by StartAll but blocked on dependencies.
  std::set<std::string> waiting_;

  mutable std::mutex mutex_;

  StateChangeCallback state_change_callback_;

  mutable Logger log_{"service-manager"};
};

}  // namespace aember::service_manager
