/**
 * @file service-manager.h
 * @author Arian Ajdari
 * @brief ServiceManager — pure coordinator over ProcessManager and
 *        ContainerManager. Owns no configs, no PIDs, no handles.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/container-manager/container-manager.h>
#include <aember-libs/process-manager/process-manager.h>
#include <aember-libs/service-manager/dependency-resolver.h>
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace aember::service_manager {

/**
 * ServiceManager is a pure coordinator.
 *
 * It owns no process handles, no container handles, no PIDs, and no configs
 * beyond what is needed to route a call to the right sub-manager.
 *
 * Responsibilities:
 *  - maintain a registry of known names and whether each is a process or
 *    container (so calls can be routed correctly)
 *  - mirror lifecycle state from sub-manager callbacks
 *  - orchestrate bulk start/stop in dependency order
 *  - dispatch ChildSupervisor PID exit events to the right sub-manager
 *  - notify callers of state changes via StateChangeCallback
 *
 * Everything else lives in ProcessManager, ContainerManager, or
 * DependencyResolver.
 */
class ServiceManager {
 public:
  using ProcessState = aember::process_manager::ProcessManager::ProcessState;
  using Logger = aember::utils::logging::Logger;

  using StateChangeCallback = std::function<void(const std::string& /*name*/,
                                                 ProcessState /*old_state*/,
                                                 ProcessState /*new_state*/)>;

  /**
   * All collaborators are injected; ServiceManager does not construct them.
   *
   * @param process_manager     Owns process spawning and PID tracking.
   * @param container_manager   Owns LXC container lifecycle and init PIDs.
   * @param dependency_resolver Evaluates start order and dependency checks.
   * @param supervisor          Delivers SIGCHLD notifications.
   */
  ServiceManager(aember::process_manager::ProcessManager& process_manager,
                 aember::container_manager::ContainerManager& container_manager,
                 DependencyResolver& dependency_resolver,
                 aember::child_supervisor::ChildSupervisor& supervisor);

  ~ServiceManager();

  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  // ---------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------

  /**
   * Register a native process by name.
   * The config must already be registered with ProcessManager::AddProcess
   * before calling this.
   */
  bool AddProcess(const std::string& name);

  /**
   * Register a container by name.
   * The config must already be registered with ContainerManager::AddContainer
   * before calling this.
   */
  bool AddContainer(const std::string& name);

  /** Deregister a process or container. Stops it first if running. */
  bool Remove(const std::string& name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  bool Start(const std::string& name);
  bool Stop(const std::string& name);
  bool Restart(const std::string& name);

  /** Start every registered entry in dependency order. */
  void StartAll();

  /** Stop every running entry in reverse dependency order. */
  void StopAll();

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool Has(const std::string& name) const;
  [[nodiscard]] bool IsContainer(const std::string& name) const;
  [[nodiscard]] ProcessState GetState(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> GetNames() const;

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /** Called by ChildSupervisor when a tracked PID exits. */
  void HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  void SetStateChangeCallback(StateChangeCallback callback);

 private:
  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  bool StartInternal(const std::string& name, bool is_restart = false);
  bool StopInternal(const std::string& name);

  void MirrorState(const std::string& name, ProcessState new_state);

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  aember::process_manager::ProcessManager& process_manager_;
  aember::container_manager::ContainerManager& container_manager_;
  DependencyResolver& dependency_resolver_;
  aember::child_supervisor::ChildSupervisor& child_supervisor_;

  /// All known names → current mirrored state.
  std::map<std::string, ProcessState> states_;

  /// Names registered as containers (the rest are processes).
  std::unordered_set<std::string> containers_;

  mutable std::mutex mutex_;

  StateChangeCallback state_change_callback_;

  mutable Logger log_{"service-manager"};
};

}  // namespace aember::service_manager
