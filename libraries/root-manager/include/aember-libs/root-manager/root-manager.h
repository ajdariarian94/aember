#pragma once

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/logging/logging.h>

#include <memory>
#include <string>

namespace aember::root_manager {

struct RootConfig {
  std::string device;         // e.g., "/dev/sda1", "UUID=xxx", "LABEL=xxx"
  std::string fstype;         // e.g., "ext4", "btrfs", "xfs"
  std::string mount_options;  // e.g., "ro,noatime"
  std::string new_root_path;  // Where to mount new root (usually "/mnt/root")

  RootConfig() : new_root_path("/mnt/root") {}
};

class RootManager {
 public:
  RootManager(aember::mount_manager::MountManager& mount_manager);
  ~RootManager();

  // Non-copyable
  RootManager(const RootManager&) = delete;
  RootManager& operator=(const RootManager&) = delete;

  // Mount the real root filesystem
  bool MountRealRoot(const RootConfig& config);

  // Perform pivot_root to switch to the new root
  bool PivotToNewRoot();

  // Complete pivot process (mount + pivot + cleanup)
  bool PerformPivot(const RootConfig& config);

  // Check if we're currently in initramfs
  static bool IsInInitramfs();

  // Get the path where new root is mounted
  std::string GetNewRootPath() const { return new_root_path_; }

 private:
  bool PrepareNewRoot();
  bool MountEssentialFilesystems();
  bool UnmountOldRoot();
  bool SwitchRoot();

  std::string ResolveDevice(const std::string& device);
  std::string FindDeviceByUUID(const std::string& uuid);
  std::string FindDeviceByLabel(const std::string& label);

  aember::mount_manager::MountManager& mount_manager_;
  std::string new_root_path_;
  bool pivoted_ = false;

  mutable aember::utils::Logger log_;
};

}  // namespace aember::root_manager
