/**
 * @file mount-manager.h
 * @author Arian Ajdari
 * @brief Library definition for MountManager
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <string>
#include <vector>

namespace aember::mount_manager {

/**
 * @brief Represents a single mount point with its properties.
 */
struct MountPoint {
  std::string source;   ///< What to mount (e.g., "proc", "none")
  std::string target;   ///< Mount point path (e.g., "/proc")
  std::string fstype;   ///< Filesystem type (e.g., "proc", "sysfs")
  unsigned long flags;  ///< Mount flags (MS_NOEXEC, MS_NOSUID, etc.)
  std::string data;     ///< Mount options (e.g., "mode=0755")

  MountPoint(const std::string& src, const std::string& tgt,
             const std::string& type, unsigned long f = 0,
             const std::string& d = "")
      : source(src), target(tgt), fstype(type), flags(f), data(d) {}
};

/**
 * @brief Manages mounting and unmounting of filesystems.
 *
 * Responsible for:
 * - Mounting early essential filesystems during init
 * - Tracking mounted targets
 * - Ensuring directories exist before mounting
 * - Unmounting filesystems safely on shutdown
 */
class MountManager {
 public:
  MountManager();
  ~MountManager();

  // Non-copyable
  MountManager(const MountManager&) = delete;
  MountManager& operator=(const MountManager&) = delete;

  /**
   * @brief Mount essential early-boot filesystems like /proc, /sys, /dev.
   * @return true if all mounts succeeded, false otherwise.
   */
  bool MountEarlyFilesystems();

  /**
   * @brief Mount a single filesystem according to MountPoint specification.
   * @param mp MountPoint object describing the mount.
   * @return true on success, false on failure.
   */
  bool Mount(const MountPoint& mp);

  bool MountSquashFS(const std::string& image, const std::string& target,
                     bool read_only = true);

  /**
   * @brief Unmount a filesystem at the given target.
   * @param target Mount point path.
   * @param force Force unmount if necessary.
   * @return true on success, false on failure.
   */
  bool Unmount(const std::string& target, bool force = false);

  /**
   * @brief Unmount all tracked filesystems in reverse order.
   * @param force Force unmount if necessary.
   */
  void UnmountAll(bool force = false);

  /**
   * @brief Check if a path is currently mounted.
   * @param target Mount point path.
   * @return true if mounted, false otherwise.
   */
  bool IsMounted(const std::string& target);

  /**
   * @brief Ensure a directory exists, creating it if needed.
   * @param path Directory path.
   * @param mode Permissions for the directory.
   * @return true if directory exists or created successfully.
   */
  bool EnsureDirectory(const std::string& path, mode_t mode = 0755);

 private:
  /**
   * @brief Provides default early filesystem mounts.
   * @return Vector of MountPoint objects for early boot.
   */
  std::vector<MountPoint> GetEarlyMounts() const;

  /**
   * @brief Check /proc/mounts to determine if a mount point is already mounted.
   * @param target Mount point path.
   * @return true if mounted, false otherwise.
   */
  bool CheckMountStatus(const std::string& target);

  aember::utils::logging::Logger log_;  ///< Logger instance
  std::vector<std::string>
      mounted_targets_;  ///< List of targets that have been mounted
};

}  // namespace aember::mount_manager
