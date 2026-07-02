/**
 * @file mount-manager.h
 * @author Arian Ajdari
 * @brief MountManager — mounts and tracks filesystems during init.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/mount/mount-point.h>

#include <string>
#include <string_view>
#include <vector>

namespace aember::mount_manager {

/**
 * Manages mounting and unmounting of filesystems.
 *
 * Responsibilities:
 *  - mount essential early-boot filesystems (/proc, /sys, /dev, etc.)
 *  - track successfully mounted targets for ordered unmount on shutdown
 *  - ensure mount point directories exist before mounting
 *  - mount SquashFS images via BusyBox
 */
class MountManager {
 public:
  using Logger = aember::utils::logging::Logger;
  using MountPoint = aember::utils::mount::MountPoint;

  MountManager();
  ~MountManager() = default;

  MountManager(const MountManager&) = delete;
  MountManager& operator=(const MountManager&) = delete;

  // ---------------------------------------------------------------------------
  // Bulk mount phases
  // ---------------------------------------------------------------------------

  /** Mount initramfs essentials (/proc, /sys, /dev, /tmp, /mnt). */
  bool MountInitramfsFilesystems();

  /** Mount early-boot filesystems (/dev/pts, /dev/shm, /run, /tmp, cgroup2). */
  bool MountEarlyFilesystems();

  // ---------------------------------------------------------------------------
  // Single-target operations
  // ---------------------------------------------------------------------------

  /** Mount a filesystem described by @p mp. */
  bool Mount(const MountPoint& mp);

  /** Mount a SquashFS image at @p target via BusyBox. */
  bool MountSquashFS(std::string_view image, std::string_view target,
                     bool read_only = true);

  /** Unmount @p target. Optionally force with MNT_FORCE. */
  bool Unmount(std::string_view target, bool force = false);

  /** Unmount all tracked targets in reverse mount order. */
  void UnmountAll(bool force = false);

  // ---------------------------------------------------------------------------
  // Utilities
  // ---------------------------------------------------------------------------

  /** Returns true if @p target appears in /proc/mounts. */
  [[nodiscard]] bool IsMounted(std::string_view target) const;

  /** Ensure @p path exists as a directory, creating it recursively if needed.
   */
  bool EnsureDirectory(std::string_view path, mode_t mode = 0755);

 private:
  [[nodiscard]] std::vector<MountPoint> GetInitramfsMounts() const;
  [[nodiscard]] std::vector<MountPoint> GetEarlyMounts() const;

  std::vector<std::string> mounted_targets_;

  mutable Logger log_{"mount-manager"};
};

}  // namespace aember::mount_manager
