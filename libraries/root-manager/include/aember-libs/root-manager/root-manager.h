/**
 * @file root-manager.h
 * @author Arian Ajdari
 * @brief RootManager — mounts the real root filesystem and performs
 *        pivot_root to switch from initramfs to the real root.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/root/root-config.h>

#include <memory>
#include <string>
#include <string_view>

namespace aember::root_manager {

class RootManager {
 public:
  using RootConfig = aember::utils::root::RootConfig;
  using Logger = aember::utils::logging::Logger;

  /**
   * @param mount_manager Shared ownership — MountManager must outlive
   *                      RootManager. Taking shared_ptr avoids dangling
   *                      reference if the caller resets its own pointer.
   */
  explicit RootManager(
      std::shared_ptr<aember::mount_manager::MountManager> mount_manager);

  ~RootManager() = default;

  RootManager(const RootManager&) = delete;
  RootManager& operator=(const RootManager&) = delete;

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  /** Complete pivot: mount real root + pivot_root + cleanup. */
  bool PerformPivot(const RootConfig& config);

  /** Mount the real root filesystem at config.new_root_path. */
  bool MountRealRoot(const RootConfig& config);

  /** Perform pivot_root to switch to the already-mounted new root. */
  bool PivotToNewRoot();

  /** Returns true if running inside initramfs (no services.json present). */
  [[nodiscard]] static bool IsInInitramfs();

  /** Returns the path where the new root is mounted. */
  [[nodiscard]] std::string_view GetNewRootPath() const {
    return new_root_path_;
  }

  /** Dump /proc/self/mountinfo to the log for diagnostics. */
  void DumpMountInfo(std::string_view reason);

  /** Resolve a symlink to its real device path. */
  [[nodiscard]] std::string ResolveSymlink(std::string_view path);

 private:
  bool PrepareNewRoot();
  bool MountEssentialFilesystems();
  bool SwitchRoot();
  bool UnmountOldRoot();

  [[nodiscard]] std::string ResolveDevice(std::string_view device);
  [[nodiscard]] std::string FindDeviceByUUID(std::string_view uuid);
  [[nodiscard]] std::string FindDeviceByLabel(std::string_view label);

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;

  std::string new_root_path_;
  bool pivoted_{false};

  mutable Logger log_{"root-manager"};
};

}  // namespace aember::root_manager
