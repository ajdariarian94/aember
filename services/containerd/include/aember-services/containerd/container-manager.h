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
#include <aember-libs/utils/logging/logging.h>
#include <aember-services/containerd/container-config.h>
#include <aember-services/containerd/container-entry.h>
#include <aember-services/containerd/container-state.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aember::containerd {

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
  using ContainerConfig = aember::containerd::ContainerConfig;
  using ContainerEntry = aember::containerd::ContainerEntry;
  using ContainerState = aember::containerd::ContainerState;
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

  /// Mounts its root file system and starts it with LXC. Different
  /// containers may start at the same time from different threads (the
  /// lock is not held while mounting and starting); the caller never
  /// starts, stops or restarts one container from two threads at once.
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

  /// After an exit (HandleExit): the delay before starting @p name again,
  /// or nullopt (policy "no", stop requested, attempts used up). Counts
  /// the attempt.
  std::optional<std::chrono::seconds> NextRestart(std::string_view name);

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

  /** The host-visible init PID of @p name while it runs, else nullopt. */
  [[nodiscard]] std::optional<pid_t> GetInitPid(std::string_view name) const;

  // ---------------------------------------------------------------------------
  // Callbacks
  // ---------------------------------------------------------------------------

  void SetStateCallback(StateCallback callback);

 private:
  /// Static BusyBox bind mounted into containers as their init wrapper.
  std::string host_busybox_{"/bin/busybox"};
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

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;

  mutable Logger log_{"container-manager"};
};

}  // namespace aember::containerd
