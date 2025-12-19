#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <string>
#include <vector>

namespace aember::mount_manager {

struct MountPoint {
  std::string source;   // What to mount (e.g., "proc", "none")
  std::string target;   // Where to mount (e.g., "/proc")
  std::string fstype;   // Filesystem type (e.g., "proc", "sysfs")
  unsigned long flags;  // Mount flags (MS_NOEXEC, MS_NOSUID, etc.)
  std::string data;     // Mount options (e.g., "mode=0755")

  MountPoint(const std::string& src, const std::string& tgt,
             const std::string& type, unsigned long f = 0,
             const std::string& d = "")
      : source(src), target(tgt), fstype(type), flags(f), data(d) {}
};

class MountManager {
 public:
  MountManager();
  ~MountManager();

  // Non-copyable
  MountManager(const MountManager&) = delete;
  MountManager& operator=(const MountManager&) = delete;

  // Mount essential early-boot filesystems
  bool MountEarlyFilesystems();

  // Mount a single filesystem
  bool Mount(const MountPoint& mp);

  // Unmount a filesystem
  bool Unmount(const std::string& target, bool force = false);

  // Unmount all tracked filesystems (in reverse order)
  void UnmountAll(bool force = false);

  // Check if a path is mounted
  bool IsMounted(const std::string& target);

  // Create directory if it doesn't exist
  bool EnsureDirectory(const std::string& path, mode_t mode = 0755);

 private:
  // Get default early filesystem mounts
  std::vector<MountPoint> GetEarlyMounts() const;

  // Parse /proc/mounts to check mount status
  bool CheckMountStatus(const std::string& target);

  aember::utils::Logger log_;
  std::vector<std::string> mounted_targets_;
};

}  // namespace aember::mount_manager
