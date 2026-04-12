#pragma once

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/logging/logging.h>

#include <memory>
#include <string>

namespace aember::root_manager {

struct RootConfig {
  std::string device;         // "/dev/sda1", "UUID=xxx", "LABEL=xxx"
  std::string fstype;         // "ext4", "btrfs", etc.
  std::string mount_options;  // "ro,noatime"
  std::string new_root_path;  // mount target (default "/mnt/root")

  RootConfig() : new_root_path("/mnt/root") {}

  void ParseFromProcCmdline(const std::string& path = "/proc/cmdline");
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

  void DumpMountInfo(const char* reason);

  std::string ResolveSymlink(const std::string& path);

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

  mutable aember::utils::logging::Logger log_;
};

}  // namespace aember::root_manager
