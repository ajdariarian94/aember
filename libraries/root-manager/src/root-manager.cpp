#include <aember-libs/root-manager/root-manager.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fstream>

namespace aember::root_manager {

RootManager::RootManager(aember::mount_manager::MountManager& mount_manager)
    : mount_manager_(mount_manager), log_("root-manager") {
  log_.info("RootManager initialized");
}

RootManager::~RootManager() = default;

bool RootManager::MountRealRoot(const RootConfig& config) {
  log_.info("Mounting real root filesystem");

  if (config.device.empty()) {
    log_.error("Root device not specified");
    return false;
  }

  if (config.fstype.empty()) {
    log_.error("Root filesystem type not specified");
    return false;
  }

  // Resolve device (handle UUID=, LABEL=, etc.)
  std::string device = ResolveDevice(config.device);
  if (device.empty()) {
    log_.error("Failed to resolve root device: {}", config.device);
    return false;
  }

  log_.info("Resolved root device: {} -> {}", config.device, device);

  // Store new root path
  new_root_path_ = config.new_root_path;

  // Create mount point
  if (!mount_manager_.EnsureDirectory(new_root_path_)) {
    log_.error("Failed to create new root directory: {}", new_root_path_);
    return false;
  }

  // Mount the real root
  log_.info(
      "Mounting {} on {} (type: {})", device, new_root_path_, config.fstype);

  unsigned long flags = 0;
  const char* options =
      config.mount_options.empty() ? nullptr : config.mount_options.c_str();

  if (mount(device.c_str(),
            new_root_path_.c_str(),
            config.fstype.c_str(),
            flags,
            options) != 0) {
    log_.error("Failed to mount real root: {}", strerror(errno));
    return false;
  }

  log_.info("Real root filesystem mounted successfully");
  return true;
}

bool RootManager::PivotToNewRoot() {
  log_.info("Pivoting to new root: {}", new_root_path_);

  if (pivoted_) {
    log_.warn("Already pivoted to new root");
    return true;
  }

  // Prepare new root
  if (!PrepareNewRoot()) { return false; }

  // Mount essential filesystems in new root
  if (!MountEssentialFilesystems()) { return false; }

  // Perform the actual pivot
  if (!SwitchRoot()) { return false; }

  // Unmount old root
  if (!UnmountOldRoot()) {
    log_.warn("Failed to unmount old root (non-fatal)");
  }

  pivoted_ = true;
  log_.info("Successfully pivoted to new root");
  return true;
}

bool RootManager::PerformPivot(const RootConfig& config) {
  log_.info("Performing complete pivot operation");

  if (!MountRealRoot(config)) { return false; }

  return PivotToNewRoot();
}

bool RootManager::PrepareNewRoot() {
  log_.info("Preparing new root");

  // Verify new root is mounted
  if (!mount_manager_.IsMounted(new_root_path_)) {
    log_.error("New root is not mounted: {}", new_root_path_);
    return false;
  }

  // Create directory for old root
  std::string old_root_dir = new_root_path_ + "/.oldroot";
  if (!mount_manager_.EnsureDirectory(old_root_dir)) {
    log_.error("Failed to create old root directory: {}", old_root_dir);
    return false;
  }

  return true;
}

bool RootManager::MountEssentialFilesystems() {
  log_.info("Mounting essential filesystems in new root");

  // Move /dev, /proc, /sys to new root
  std::vector<std::pair<std::string, std::string>> moves = {
      {"/dev", new_root_path_ + "/dev"},
      {"/proc", new_root_path_ + "/proc"},
      {"/sys", new_root_path_ + "/sys"}};

  for (const auto& [src, dst] : moves) {
    // Create target directory
    if (!mount_manager_.EnsureDirectory(dst)) {
      log_.error("Failed to create directory: {}", dst);
      return false;
    }

    // Move mount
    log_.debug("Moving {} to {}", src, dst);
    if (mount(src.c_str(), dst.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
      log_.error("Failed to move {} to {}: {}", src, dst, strerror(errno));
      return false;
    }
  }

  log_.info("Essential filesystems moved to new root");
  return true;
}

bool RootManager::SwitchRoot() {
  log_.info("Switching to new root");

  // Change to new root directory
  if (chdir(new_root_path_.c_str()) != 0) {
    log_.error("Failed to chdir to new root: {}", strerror(errno));
    return false;
  }

  // Perform pivot_root
  // pivot_root(new_root, put_old)
  std::string put_old = ".oldroot";

  if (syscall(SYS_pivot_root, ".", put_old.c_str()) != 0) {
    log_.error("pivot_root failed: {}", strerror(errno));
    return false;
  }

  // Change root to new root
  if (chroot(".") != 0) {
    log_.error("Failed to chroot: {}", strerror(errno));
    return false;
  }

  // Change to root directory
  if (chdir("/") != 0) {
    log_.error("Failed to chdir to /: {}", strerror(errno));
    return false;
  }

  log_.info("Successfully switched to new root");
  return true;
}

bool RootManager::UnmountOldRoot() {
  log_.info("Unmounting old root");

  std::string old_root = "/.oldroot";

  // Try to unmount everything under old root
  if (umount2(old_root.c_str(), MNT_DETACH) != 0) {
    log_.warn("Failed to unmount old root: {}", strerror(errno));
    return false;
  }

  // Remove old root directory
  if (rmdir(old_root.c_str()) != 0) {
    log_.debug("Failed to remove old root directory: {}", strerror(errno));
  }

  log_.info("Old root unmounted");
  return true;
}

std::string RootManager::ResolveDevice(const std::string& device) {
  // Check if it's a UUID
  if (device.substr(0, 5) == "UUID=") {
    std::string uuid = device.substr(5);
    return FindDeviceByUUID(uuid);
  }

  // Check if it's a LABEL
  if (device.substr(0, 6) == "LABEL=") {
    std::string label = device.substr(6);
    return FindDeviceByLabel(label);
  }

  // Assume it's a direct device path
  return device;
}

std::string RootManager::FindDeviceByUUID(const std::string& uuid) {
  log_.debug("Looking for device with UUID: {}", uuid);

  std::string path = "/dev/disk/by-uuid/" + uuid;

  // Check if symlink exists
  struct stat st;
  if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
    char resolved[PATH_MAX];
    ssize_t len = readlink(path.c_str(), resolved, sizeof(resolved) - 1);
    if (len > 0) {
      resolved[len] = '\0';

      // Convert relative path to absolute
      if (resolved[0] != '/') {
        std::string abs_path = "/dev/disk/by-uuid/" + std::string(resolved);
        char real_path[PATH_MAX];
        if (realpath(abs_path.c_str(), real_path) != nullptr) {
          return std::string(real_path);
        }
      } else {
        return std::string(resolved);
      }
    }
  }

  log_.warn("Device with UUID {} not found", uuid);
  return "";
}

std::string RootManager::FindDeviceByLabel(const std::string& label) {
  log_.debug("Looking for device with LABEL: {}", label);

  std::string path = "/dev/disk/by-label/" + label;

  // Check if symlink exists
  struct stat st;
  if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
    char resolved[PATH_MAX];
    ssize_t len = readlink(path.c_str(), resolved, sizeof(resolved) - 1);
    if (len > 0) {
      resolved[len] = '\0';

      // Convert relative path to absolute
      if (resolved[0] != '/') {
        std::string abs_path = "/dev/disk/by-label/" + std::string(resolved);
        char real_path[PATH_MAX];
        if (realpath(abs_path.c_str(), real_path) != nullptr) {
          return std::string(real_path);
        }
      } else {
        return std::string(resolved);
      }
    }
  }

  log_.warn("Device with LABEL {} not found", label);
  return "";
}

bool RootManager::IsInInitramfs() {
  // Check if we're running from initramfs by looking at rootfs type
  std::ifstream mounts("/proc/mounts");
  std::string line;

  while (std::getline(mounts, line)) {
    if (line.find("/ rootfs") != std::string::npos ||
        line.find("/ tmpfs") != std::string::npos) {
      return true;
    }
  }

  return false;
}

}  // namespace aember::root_manager
