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
#include <utility>
#include <vector>

namespace aember::container_manager {

/// LXC's config path for aember's containers.
inline constexpr const char* kLxcPath = "/var/lib/aember/containers";

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

  /** Take over a container that is already running (left by a previous
   *  containerd): LXC handle, state running, init pid tracked. False if
   *  it isn't running (start it normally then). */
  bool Adopt(std::string_view name);

  /** Stop every running container (graceful, then forced). */
  void StopAll();

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

  /// LXC settings applied to every container after its own config, one
  /// "key = value" per line (default /etc/aember/lxc-extra.conf; a missing
  /// file means none). Used e.g. to pass a display into containers.
  void SetExtraConfigPath(std::string path) {
    extra_config_path_ = std::move(path);
  }

  /// Parse "key = value" lines ('#' comments, blank lines and lines without
  /// '=' are skipped).
  static std::vector<std::pair<std::string, std::string>> ParseExtraConfig(
      std::string_view text);

 private:
  std::string extra_config_path_{"/etc/aember/lxc-extra.conf"};
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
