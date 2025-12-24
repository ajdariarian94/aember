#include <aember-libs/root-manager/root-manager.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>
#include <vector>

namespace aember::root_manager {

static std::string ErrnoString(const char* action) {
  return std::string(action) + ": errno=" + std::to_string(errno) + " (" +
         strerror(errno) + ")";
}

RootManager::RootManager(aember::mount_manager::MountManager& mount_manager)
    : mount_manager_(mount_manager), log_("root-manager") {
  log_.info("RootManager initialized");
}

RootManager::~RootManager() = default;

bool RootManager::PerformPivot(const RootConfig& config) {
  log_.info("Starting complete root pivot operation");

  // Make mount propagation private (CRITICAL)
  log_.info("Setting mount propagation to MS_PRIVATE");
  if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    log_.error("Failed to set MS_PRIVATE: {}", ErrnoString("mount"));
    return false;
  }

  if (!MountRealRoot(config)) { return false; }

  return PivotToNewRoot();
}

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

  std::string device = ResolveDevice(config.device);
  if (device.empty()) {
    log_.error("Failed to resolve root device: {}", config.device);
    return false;
  }

  log_.info("Resolved root device: {} -> {}", config.device, device);

  new_root_path_ = config.new_root_path;

  if (!mount_manager_.EnsureDirectory(new_root_path_)) {
    log_.error("Failed to create new root directory: {}", new_root_path_);
    return false;
  }

  const char* options =
      config.mount_options.empty() ? nullptr : config.mount_options.c_str();

  log_.info(
      "Mounting {} on {} (type={})", device, new_root_path_, config.fstype);

  if (mount(device.c_str(),
            new_root_path_.c_str(),
            config.fstype.c_str(),
            0,
            options) != 0) {
    log_.error("Failed to mount real root: {}", ErrnoString("mount"));
    DumpMountInfo("mount real root failed");
    return false;
  }

  return true;
}

bool RootManager::PivotToNewRoot() {
  log_.info("Pivoting to new root: {}", new_root_path_);

  if (pivoted_) {
    log_.warn("Already pivoted");
    return true;
  }

  if (!PrepareNewRoot()) { return false; }

  if (!MountEssentialFilesystems()) { return false; }

  if (!SwitchRoot()) { return false; }

  if (!UnmountOldRoot()) {
    log_.warn("Failed to unmount old root (non-fatal)");
  }

  pivoted_ = true;
  log_.info("Root pivot completed successfully");
  return true;
}

bool RootManager::PrepareNewRoot() {
  log_.info("Preparing new root");

  if (!mount_manager_.IsMounted(new_root_path_)) {
    log_.error("New root is not a mount point: {}", new_root_path_);
    DumpMountInfo("new root not mounted");
    return false;
  }

  std::string old_root_dir = new_root_path_ + "/.oldroot";
  if (!mount_manager_.EnsureDirectory(old_root_dir)) {
    log_.error("Failed to create {}", old_root_dir);
    return false;
  }

  // Ensure .oldroot is empty
  DIR* d = opendir(old_root_dir.c_str());
  if (!d) {
    log_.error("Failed to open {}: {}", old_root_dir, ErrnoString("opendir"));
    return false;
  }

  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      log_.error("{} is not empty, cannot pivot_root", old_root_dir);
      closedir(d);
      return false;
    }
  }
  closedir(d);

  return true;
}

bool RootManager::MountEssentialFilesystems() {
  log_.info("Moving essential filesystems");

  struct Move {
    const char* src;
    std::string dst;
  };

  std::vector<Move> moves = {
      {"/dev", new_root_path_ + "/dev"},
      {"/proc", new_root_path_ + "/proc"},
      {"/sys", new_root_path_ + "/sys"},
  };

  for (const auto& m : moves) {
    if (!mount_manager_.EnsureDirectory(m.dst)) {
      log_.error("Failed to create {}", m.dst);
      return false;
    }

    log_.info("MS_MOVE {} -> {}", m.src, m.dst);
    if (mount(m.src, m.dst.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
      log_.error("Failed to move {}: {}", m.src, ErrnoString("mount"));
      DumpMountInfo("MS_MOVE failed");
      return false;
    }
  }

  return true;
}

bool RootManager::SwitchRoot() {
  log_.info("Executing pivot_root");

  if (chdir(new_root_path_.c_str()) != 0) {
    log_.error("chdir failed: {}", ErrnoString("chdir"));
    return false;
  }

  if (syscall(SYS_pivot_root, ".", ".oldroot") != 0) {
    log_.error("pivot_root failed: {}", ErrnoString("pivot_root"));
    DumpMountInfo("pivot_root failed");
    return false;
  }

  if (chroot(".") != 0) {
    log_.error("chroot failed: {}", ErrnoString("chroot"));
    return false;
  }

  if (chdir("/") != 0) {
    log_.error("chdir(/) failed: {}", ErrnoString("chdir"));
    return false;
  }

  return true;
}

bool RootManager::UnmountOldRoot() {
  log_.info("Unmounting old root");

  const char* old_root = "/.oldroot";

  if (umount2(old_root, MNT_DETACH) != 0) {
    log_.warn("Failed to unmount old root: {}", ErrnoString("umount2"));
    return false;
  }

  if (rmdir(old_root) != 0) {
    log_.debug("Failed to remove old root dir: {}", ErrnoString("rmdir"));
  }

  return true;
}

std::string RootManager::ResolveDevice(const std::string& device) {
  if (device.rfind("UUID=", 0) == 0) {
    return FindDeviceByUUID(device.substr(5));
  }

  if (device.rfind("LABEL=", 0) == 0) {
    return FindDeviceByLabel(device.substr(6));
  }

  return device;
}

std::string RootManager::FindDeviceByUUID(const std::string& uuid) {
  log_.debug("Searching device by UUID={}", uuid);
  return ResolveSymlink("/dev/disk/by-uuid/" + uuid);
}

std::string RootManager::FindDeviceByLabel(const std::string& label) {
  log_.debug("Searching device by LABEL={}", label);
  return ResolveSymlink("/dev/disk/by-label/" + label);
}

std::string RootManager::ResolveSymlink(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) { return {}; }

  char link[PATH_MAX];
  ssize_t len = readlink(path.c_str(), link, sizeof(link) - 1);
  if (len <= 0) { return {}; }
  link[len] = '\0';

  char real[PATH_MAX];
  if (realpath(path.c_str(), real)) { return real; }

  return {};
}

bool RootManager::IsInInitramfs() {
  std::ifstream mounts("/proc/self/mountinfo");
  std::string line;

  while (std::getline(mounts, line)) {
    if (line.find(" - rootfs ") != std::string::npos ||
        line.find(" - tmpfs ") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RootManager::DumpMountInfo(const char* reason) {
  log_.error("Dumping mountinfo due to: {}", reason);

  std::ifstream f("/proc/self/mountinfo");
  std::string line;
  while (std::getline(f, line)) { log_.error("mountinfo: {}", line); }
}

}  // namespace aember::root_manager
