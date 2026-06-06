/**
 * @file mount-manager.cpp
 * @author Arian Ajdari
 * @brief Library implementation for MountManager
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/mount-manager/mount-manager.h>

#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>

namespace aember::mount_manager {

/**
 * @brief Construct a MountManager instance.
 */
MountManager::MountManager() : log_("mount-manager") {
  log_.info("MountManager initialized");
}

/**
 * @brief Destructor.
 *
 * Note: Does not automatically unmount filesystems. Call UnmountAll()
 * explicitly if needed.
 */
MountManager::~MountManager() {
  // Destructor intentionally left empty
}

/**
 * @brief Returns a list of essential early-boot filesystem mounts.
 */
std::vector<MountManager::MountPoint> MountManager::GetEarlyMounts() const {
  std::vector<MountManager::MountPoint> mounts;

  // /dev/pts - pseudo-terminals
  mounts.emplace_back("devpts",
                      "/dev/pts",
                      "devpts",
                      MS_NOEXEC | MS_NOSUID,
                      "mode=0620,gid=5,ptmxmode=0666");

  // /dev/shm - shared memory
  mounts.emplace_back("tmpfs",
                      "/dev/shm",
                      "tmpfs",
                      MS_NOEXEC | MS_NOSUID | MS_NODEV,
                      "mode=1777");

  // /run - runtime files
  mounts.emplace_back("tmpfs",
                      "/run",
                      "tmpfs",
                      MS_NOEXEC | MS_NOSUID | MS_NODEV,
                      "mode=0755,size=10%");

  // /tmp - temporary files
  mounts.emplace_back(
      "tmpfs", "/tmp", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, "mode=1777");

  // /sys/fs/cgroup - cgroup2 filesystem
  mounts.emplace_back("cgroup2",
                      "/sys/fs/cgroup",
                      "cgroup2",
                      MS_NOEXEC | MS_NOSUID | MS_NODEV,
                      "nsdelegate");

  return mounts;
}

/**
 * @brief Ensure a directory exists, creating it recursively if necessary.
 *
 * @param path Directory path.
 * @param mode Permissions for newly created directories.
 * @return true if the directory exists or was created successfully.
 */
bool MountManager::EnsureDirectory(const std::string& path, mode_t mode) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return true;  // Directory already exists
    } else {
      log_.error("Path '{}' exists but is not a directory", path);
      return false;
    }
  }

  // Create directories recursively
  std::string current_path;
  for (size_t i = 0; i < path.length(); ++i) {
    if (path[i] == '/' && i > 0) {
      if (stat(current_path.c_str(), &st) != 0) {
        if (mkdir(current_path.c_str(), mode) != 0 && errno != EEXIST) {
          log_.error("Failed to create directory '{}': {}",
                     current_path,
                     strerror(errno));
          return false;
        }
      }
    }
    current_path += path[i];
  }

  // Create final directory
  if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
    log_.error("Failed to create directory '{}': {}", path, strerror(errno));
    return false;
  }

  log_.debug("Created directory '{}'", path);
  return true;
}

/**
 * @brief Mount a filesystem described by MountPoint.
 *
 * @param mp MountPoint object.
 * @return true on success, false otherwise.
 */
bool MountManager::Mount(const MountPoint& mp) {
  // Skip if already mounted
  if (IsMounted(mp.target)) {
    log_.debug("'{}' is already mounted, skipping", mp.target);
    return true;
  }

  // Ensure mount point directory exists
  if (!EnsureDirectory(mp.target)) { return false; }

  log_.info("Mounting {} on {} (type: {})", mp.source, mp.target, mp.fstype);

  const char* data_ptr = mp.data.empty() ? nullptr : mp.data.c_str();

  if (mount(mp.source.c_str(),
            mp.target.c_str(),
            mp.fstype.c_str(),
            mp.flags,
            data_ptr) != 0) {
    log_.error(
        "Failed to mount {} on {}: {}", mp.source, mp.target, strerror(errno));
    return false;
  }

  // Track successfully mounted filesystem
  mounted_targets_.push_back(mp.target);

  log_.info("Successfully mounted {} on {}", mp.source, mp.target);
  return true;
}

bool MountManager::MountSquashFS(const std::string& image,
                                 const std::string& target, bool read_only) {
  if (IsMounted(target)) {
    log_.debug("'{}' is already mounted, skipping", target);
    return true;
  }

  if (!EnsureDirectory(target)) { return false; }

  log_.info("Mounting {} on {} using BusyBox", image, target);

  // Build command string for squashfs mount
  std::string cmd =
      "/bin/busybox mount -t squashfs " + image + " " + target + " -o async";

  int ret = system(cmd.c_str());
  if (ret != 0) {
    log_.error("BusyBox mount failed for {} on {}", image, target);
    return false;
  }

  mounted_targets_.push_back(target);
  log_.info("Successfully mounted {} on {} via BusyBox", image, target);

  // Mount tmpfs over /tmp to make it writable
  std::string tmp_path = target + "/tmp";
  if (!EnsureDirectory(tmp_path)) {
    log_.warn("Could not ensure tmp directory exists at {}", tmp_path);
  } else {
    std::string tmp_cmd = "/bin/busybox mount -t tmpfs tmpfs " + tmp_path;
    ret = system(tmp_cmd.c_str());
    if (ret != 0) {
      log_.warn("Failed to mount tmpfs on {}", tmp_path);
    } else {
      mounted_targets_.push_back(tmp_path);
      log_.info("Successfully mounted tmpfs on {}", tmp_path);
    }
  }

  return true;
}

std::vector<MountManager::MountPoint> MountManager::GetInitramfsMounts() const {
  return {
      {"proc", "/proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, ""},
      {"sysfs", "/sys", "sysfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, ""},
      {"devtmpfs", "/dev", "devtmpfs", MS_NOSUID, ""},
      {"tmpfs", "/tmp", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, "mode=1777"},
      {"tmpfs", "/mnt", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, ""},
  };
}

bool MountManager::MountInitramfsFilesystems() {
  log_.info("Mounting initramfs filesystems...");

  for (const auto& mp : GetInitramfsMounts()) {
    if (!Mount(mp)) {
      log_.error("Critical: failed to mount {}", mp.target);
      return false;
    }
  }

  log_.info("Initramfs filesystems mounted successfully");
  return true;
}

/**
 * @brief Mount all early boot filesystems.
 *
 * @return true if all mounts succeeded, false if any failed.
 */
bool MountManager::MountEarlyFilesystems() {
  log_.info("Mounting early boot filesystems...");

  auto mounts = GetEarlyMounts();
  bool all_success = true;

  for (const auto& mp : mounts) {
    if (!Mount(mp)) {
      log_.warn("Failed to mount {}, continuing anyway", mp.target);
      all_success = false;
    }
  }

  if (all_success) {
    log_.info("All early filesystems mounted successfully");
  } else {
    log_.warn("Some early filesystems failed to mount");
  }

  return all_success;
}

/**
 * @brief Unmount a filesystem.
 *
 * @param target Mount point path.
 * @param force Force unmount if necessary.
 * @return true on success, false on failure.
 */
bool MountManager::Unmount(const std::string& target, bool force) {
  log_.info("Unmounting {}{}", target, force ? " (forced)" : "");

  int flags = force ? MNT_FORCE : 0;

  if (umount2(target.c_str(), flags) != 0) {
    if (errno == EINVAL || errno == ENOENT) {
      log_.debug("{} was not mounted", target);
      return true;
    }
    log_.error("Failed to unmount {}: {}", target, strerror(errno));
    return false;
  }

  // Remove from tracked mounts
  auto it = std::find(mounted_targets_.begin(), mounted_targets_.end(), target);
  if (it != mounted_targets_.end()) { mounted_targets_.erase(it); }

  log_.info("Successfully unmounted {}", target);
  return true;
}

/**
 * @brief Unmount all tracked filesystems in reverse order.
 *
 * @param force Force unmount if necessary.
 */
void MountManager::UnmountAll(bool force) {
  log_.info("Unmounting all tracked filesystems...");

  // Reverse order to respect dependency hierarchy
  for (auto it = mounted_targets_.rbegin(); it != mounted_targets_.rend();
       ++it) {
    Unmount(*it, force);
  }

  mounted_targets_.clear();
}

/**
 * @brief Check if a target is mounted.
 *
 * @param target Mount point path.
 * @return true if mounted, false otherwise.
 */
bool MountManager::IsMounted(const std::string& target) {
  return CheckMountStatus(target);
}

/**
 * @brief Inspect /proc/mounts to determine if a mount point is currently
 * mounted.
 *
 * @param target Mount point path.
 * @return true if mounted, false otherwise.
 */
bool MountManager::CheckMountStatus(const std::string& target) {
  std::ifstream mounts("/proc/mounts");
  if (!mounts.is_open()) {
    log_.warn("Could not open /proc/mounts to check mount status");
    return false;
  }

  std::string line;
  while (std::getline(mounts, line)) {
    // Format: device mountpoint fstype options dump pass
    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) continue;

    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) continue;

    std::string mountpoint =
        line.substr(first_space + 1, second_space - first_space - 1);

    // Handle escaped spaces (\040)
    size_t pos = 0;
    while ((pos = mountpoint.find("\\040", pos)) != std::string::npos) {
      mountpoint.replace(pos, 4, " ");
      pos += 1;
    }

    if (mountpoint == target) { return true; }
  }

  return false;
}

}  // namespace aember::mount_manager
