/**
 * @file container-manager.h
 * @author Arian Ajdari
 * @brief ContainerManager — manages LXC container lifecycle, including the
 *        container init PIDs that are visible from the host.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/container/container-entry.h>
#include <aember-libs/utils/container/container-parser.h>
#include <aember-libs/utils/container/container-state.h>
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aember::container_manager {

/**
 * ContainerManager owns the full lifecycle of LXC containers.
 *
 * When LXC starts a container it creates an init process inside the container
 * namespace. That process has a PID visible from the host — ContainerManager
 * tracks those PIDs. ProcessManager must never see them.
 *
 * PID dispatch contract (with ServiceManager):
 *   ServiceManager::HandleServiceExit calls HandleExit on both managers in
 *   order. The one that owns the PID returns true and cleans up; the other
 *   returns false. Callers must not assume ownership without checking the
 *   return value.
 */
class ContainerManager {
 public:
  using ContainerConfig = aember::utils::container::ContainerConfig;
  using ContainerEntry = aember::utils::container::ContainerEntry;
  using ContainerState = aember::utils::container::ContainerState;
  using Logger = aember::utils::logging::Logger;

  /**
   * Fired when a container transitions between states.
   *
   * @param name      Container name.
   * @param old_state Previous state.
   * @param new_state New state.
   */
  using StateCallback = std::function<void(const std::string& /*name*/,
                                           ContainerState /*old_state*/,
                                           ContainerState /*new_state*/)>;

  explicit ContainerManager(
      std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
      StateCallback cb = nullptr);
  ~ContainerManager();

  ContainerManager(const ContainerManager&) = delete;
  ContainerManager& operator=(const ContainerManager&) = delete;

  // ---------------------------------------------------------------------------
  // Registry
  // ---------------------------------------------------------------------------

  bool AddContainer(const ContainerConfig& config);
  bool RemoveContainer(const std::string& name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  bool StartContainer(const std::string& name);
  bool StopContainer(const std::string& name);

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /**
   * Called by ServiceManager when ChildSupervisor delivers an exit event.
   *
   * Returns true if @p pid belongs to a container tracked here (entry is
   * cleaned up and StateCallback fired). Returns false if the PID is
   * unknown — the caller should then try ProcessManager::HandleExit.
   */
  bool HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool HasContainer(const std::string& name) const;
  [[nodiscard]] ContainerState GetContainerState(const std::string& name) const;
  [[nodiscard]] bool IsRunning(const std::string& name) const;

  /** Returns the host-visible init PID for @p name, or nullopt if not running.
   */
  [[nodiscard]] std::optional<pid_t> GetInitPid(const std::string& name) const;

  std::vector<ContainerConfig> LoadContainers(const std::string& source);

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  /** Replace the state-change callback after construction. */
  void SetStateCallback(StateCallback callback);

 private:
  // ---------------------------------------------------------------------------
  // LXC lifecycle helpers
  // ---------------------------------------------------------------------------

  bool Create(ContainerEntry& e);
  bool Start(ContainerEntry& e);
  bool Stop(ContainerEntry& e);
  void Release(ContainerEntry& e);

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  void SetState(ContainerEntry& e, ContainerState s);

  ContainerEntry* Find(const std::string& name);
  const ContainerEntry* Find(const std::string& name) const;

  // ---------------------------------------------------------------------------
  // Members
  // ---------------------------------------------------------------------------

  std::vector<ContainerEntry> containers_;
  std::map<pid_t, std::string> pid_to_name_;  ///< container init PIDs only

  mutable std::mutex mutex_;

  StateCallback callback_;

  aember::utils::container::ContainersConfigParser parser_{};

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;

  mutable Logger log_{"container-manager"};
};

}  // namespace aember::container_manager
