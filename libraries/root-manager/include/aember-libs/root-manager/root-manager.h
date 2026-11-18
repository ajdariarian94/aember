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

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace aember::root_manager {

/// Program exec'd as PID 1 in the new root.
inline constexpr const char* kRootInit = "/usr/bin/aember";

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

  /// Label and UUID of an ext2/3/4 filesystem, read from its superblock.
  struct Ext4Id {
    std::string label;
    std::string uuid;  ///< lower case, 8-4-4-4-12
  };
  /// nullopt if @p path holds no ext2/3/4 filesystem.
  static std::optional<Ext4Id> ReadExt4Id(const std::string& path);

  /// Block device whose ext4 LABEL or UUID (@p key) is @p value, by reading
  /// the superblocks of every device in @p sys_block (no udev needed: aember
  /// has no /dev/disk/by-label). "" if none.
  static std::string FindBlockDevice(
      std::string_view key, std::string_view value,
      const std::string& sys_block = "/sys/class/block",
      const std::string& dev_dir = "/dev");

  /// How long ResolveDevice waits for the root device (rootwait).
  static constexpr std::chrono::seconds kRootWait{30};
  void SetRootWait(std::chrono::milliseconds wait) { root_wait_ = wait; }

  /// Delete everything under @p dir_fd that lives on @p root_dev (the
  /// initramfs); other filesystems (mount points) and symlink targets are
  /// left alone. Used by SwitchRoot to free the initramfs RAM.
  static void DeleteInitramfs(int dir_fd, dev_t root_dev);

  /** Returns the path where the new root is mounted. */
  [[nodiscard]] std::string_view GetNewRootPath() const {
    return new_root_path_;
  }

  /** Dump /proc/self/mountinfo to the log for diagnostics. */
  void DumpMountInfo(std::string_view reason);

 private:
  bool PrepareNewRoot();
  bool MountEssentialFilesystems();
  bool SwitchRoot();
  bool UnmountOldRoot();

  [[nodiscard]] std::string ResolveDevice(std::string_view device);

  std::shared_ptr<aember::mount_manager::MountManager> mount_manager_;

  std::string new_root_path_;
  bool pivoted_{false};
  std::chrono::milliseconds root_wait_{kRootWait};

  mutable Logger log_{"root-manager"};
};

}  // namespace aember::root_manager
