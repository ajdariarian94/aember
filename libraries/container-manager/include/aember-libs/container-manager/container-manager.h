/**
 * @file container_manager.h
 * @author Arian Ajdari
 * @brief Minimal ContainerManager - manages basic LXC container lifecycle
 *        for PID1 (simplified version).
 * @version 0.1
 * @date 2026-03-23
 */

#pragma once

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/container/container-entry.h>
#include <aember-libs/utils/container/container-state.h>
#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aember::container_manager {

// ---------------------------------------------------------------------------
// ContainerManager
// ---------------------------------------------------------------------------

class ContainerManager {
 public:
  using ContainerConfig = aember::utils::container::ContainerConfig;
  using ContainerEntry = aember::utils::container::ContainerEntry;
  using ContainerState = aember::utils::container::ContainerState;
  using Logger = aember::utils::logging::Logger;

  using StateCallback =
      std::function<void(const std::string&, ContainerState, ContainerState)>;

  explicit ContainerManager(
      std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
      StateCallback cb = nullptr);
  ~ContainerManager();

  ContainerManager(const ContainerManager&) = delete;
  ContainerManager& operator=(const ContainerManager&) = delete;

  // --------------------------
  // Container management
  // --------------------------
  bool AddContainer(const ContainerConfig& config);
  bool RemoveContainer(const std::string& name);

  bool StartContainer(const std::string& name);
  bool StopContainer(const std::string& name);

  // --------------------------
  // Queries
  // --------------------------
  ContainerState GetContainerState(const std::string& name) const;
  bool IsRunning(const std::string& name) const;

 private:
  // LXC lifecycle
  bool Create(ContainerEntry& e);
  bool Start(ContainerEntry& e);
  bool Stop(ContainerEntry& e);
  void Release(ContainerEntry& e);

  // Helpers
  void SetState(ContainerEntry& e, ContainerState s);

  ContainerEntry* Find(const std::string& name);
  const ContainerEntry* Find(const std::string& name) const;

 private:
  std::vector<ContainerEntry> containers_;
  mutable std::mutex mutex_;

  StateCallback callback_;

  Logger log_{"container-manager"};

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;
};

}  // namespace aember::container_manager
