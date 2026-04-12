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
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/container/container-state.h>
#include <aember-libs/utils/container/container-entry.h>

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
  using StateCallback =
      std::function<void(const std::string&, aember::utils::container::ContainerState, aember::utils::container::ContainerState)>;

  explicit ContainerManager(
      std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
      StateCallback cb = nullptr);
  ~ContainerManager();

  ContainerManager(const ContainerManager&) = delete;
  ContainerManager& operator=(const ContainerManager&) = delete;

  // --------------------------
  // Container management
  // --------------------------
  bool AddContainer(const aember::utils::container::ContainerConfig& config);
  bool RemoveContainer(const std::string& name);

  bool StartContainer(const std::string& name);
  bool StopContainer(const std::string& name);

  // --------------------------
  // Queries
  // --------------------------
  aember::utils::container::ContainerState GetContainerState(const std::string& name) const;
  bool IsRunning(const std::string& name) const;

 private:
  // LXC lifecycle
  bool Create(aember::utils::container::ContainerEntry& e);
  bool Start(aember::utils::container::ContainerEntry& e);
  bool Stop(aember::utils::container::ContainerEntry& e);
  void Release(aember::utils::container::ContainerEntry& e);

  // Helpers
  void SetState(aember::utils::container::ContainerEntry& e, aember::utils::container::ContainerState s);

  aember::utils::container::ContainerEntry* Find(const std::string& name);
  const aember::utils::container::ContainerEntry* Find(const std::string& name) const;

 private:
  std::vector<aember::utils::container::ContainerEntry> containers_;
  mutable std::mutex mutex_;

  StateCallback callback_;

  aember::utils::logging::Logger log_{"container-manager"};

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;
};

}  // namespace aember::container_manager
