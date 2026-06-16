/**
 * @file container-manager.h
 * @author Arian Ajdari
 * @brief ContainerManager — manages LXC container lifecycle, including the
 *        container init PIDs that are visible from the host.
 * @version 0.3
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
 *   ServiceManager::HandleExit calls HandleExit on both managers in order.
 *   The one that owns the PID returns true and cleans up; the other returns
 *   false.
 */
class ContainerManager {
 public:
  using ContainerConfig = aember::utils::container::ContainerConfig;
  using ContainerEntry = aember::utils::container::ContainerEntry;
  using ContainerState = aember::utils::container::ContainerState;
  using Logger = aember::utils::logging::Logger;

  /// move_only_function — callbacks are never copied.
  using StateCallback = std::move_only_function<void(
      const std::string& /*name*/, ContainerState /*old_state*/,
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
  bool RemoveContainer(std::string_view name);

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  bool StartContainer(std::string_view name);
  bool StopContainer(std::string_view name);

  // ---------------------------------------------------------------------------
  // SIGCHLD integration
  // ---------------------------------------------------------------------------

  /**
   * Called by ServiceManager when ChildSupervisor delivers an exit event.
   * Returns true if @p pid belongs to a container tracked here.
   * Returns false if unknown — caller should try ProcessManager::HandleExit.
   */
  bool HandleExit(pid_t pid, int exit_code);

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool HasContainer(std::string_view name) const;
  [[nodiscard]] ContainerState GetContainerState(std::string_view name) const;
  [[nodiscard]] bool IsRunning(std::string_view name) const;

  /** Returns the host-visible init PID for @p name, or nullopt if not running.
   */
  [[nodiscard]] std::optional<pid_t> GetInitPid(std::string_view name) const;

  std::vector<ContainerConfig> LoadContainers(std::string_view source);

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

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

  ContainerEntry* Find(std::string_view name);
  const ContainerEntry* Find(std::string_view name) const;

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
