/**
 * @file service-manager.h
 * @author Arian Ajdari
 * @brief ServiceManager — pure coordinator over ProcessManager and
 *        ContainerManager. Owns no configs, no PIDs, no handles.
 * @version 0.4
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
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aember::service_manager {

/**
 * ServiceManager is a pure coordinator.
 *
 * Owns no process handles, no container handles, no PIDs, no configs.
 *
 * Responsibilities:
 *  - registry of known names and whether each is a process or container
 *  - mirror lifecycle state from sub-manager callbacks
 *  - orchestrate bulk start/stop in dependency order
 *  - dispatch ChildSupervisor PID exit events to the right sub-manager
 *  - notify callers of state changes via StateChangeCallback
 */
class ServiceManager {
 public:
  using ProcessState = aember::process_manager::ProcessManager::ProcessState;
  using Logger = aember::utils::logging::Logger;

  /// move_only_function — never copied.
  using StateChangeCallback = std::move_only_function<void(
      const std::string& /*name*/, ProcessState /*old_state*/,
      ProcessState /*new_state*/)>;

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

  /** Config must already be in ProcessManager before calling this. */
  bool AddProcess(std::string_view name);

  /** Config must already be in ContainerManager before calling this. */
  bool AddContainer(std::string_view name);

  /** Deregister. Stops first if running. */
  bool Remove(std::string_view name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  bool Start(std::string_view name);
  bool Stop(std::string_view name);
  bool Restart(std::string_view name);

  void StartAll();
  void StopAll();

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool Has(std::string_view name) const;
  [[nodiscard]] bool IsContainer(std::string_view name) const;
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

  void MirrorState(const std::string& name, ProcessState new_state);

 private:
  bool StartInternal(std::string_view name, bool is_restart = false);
  bool StopInternal(std::string_view name);

  aember::process_manager::ProcessManager& process_manager_;
  aember::container_manager::ContainerManager& container_manager_;
  DependencyResolver& dependency_resolver_;
  aember::child_supervisor::ChildSupervisor& child_supervisor_;

  std::map<std::string, ProcessState> states_;
  std::unordered_set<std::string> containers_;

  mutable std::mutex mutex_;

  StateChangeCallback state_change_callback_;

  mutable Logger log_{"service-manager"};
};

}  // namespace aember::service_manager
