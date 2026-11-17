/**
 * @file root-manager.cpp
 * @author Arian Ajdari
 * @brief RootManager implementation.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/root-manager/root-manager.h>

#include <array>
#include <format>
#include <fstream>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace aember::root_manager {

// ---------------------------------------------------------------------------
// Ctor
// ---------------------------------------------------------------------------

RootManager::RootManager(
    std::shared_ptr<aember::mount_manager::MountManager> mount_manager)
    : mount_manager_(std::move(mount_manager)) {
  log_.info("RootManager initialized");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool RootManager::PerformPivot(const RootConfig& config) {
  log_.info("Starting root pivot operation");

  if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    log_.error(
        "Failed to set MS_PRIVATE: errno={} ({})", errno, strerror(errno));
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

  const auto device = ResolveDevice(config.device);
  if (device.empty()) {
    log_.error("Failed to resolve root device: {}", config.device);
    return false;
  }

  log_.info("Resolved root device: {} -> {}", config.device, device);

  new_root_path_ = config.new_root_path;

  if (!mount_manager_->EnsureDirectory(new_root_path_)) {
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
    log_.error(
        "Failed to mount real root: errno={} ({})", errno, strerror(errno));
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

// ---------------------------------------------------------------------------
// Private — pivot stages
// ---------------------------------------------------------------------------

bool RootManager::PrepareNewRoot() {
  log_.info("Preparing new root");

  if (!mount_manager_->IsMounted(new_root_path_)) {
    log_.error("New root is not a mount point: {}", new_root_path_);
    DumpMountInfo("new root not mounted");
    return false;
  }

  const auto old_root_dir = std::format("{}/.oldroot", new_root_path_);

  if (!mount_manager_->EnsureDirectory(old_root_dir)) {
    log_.error("Failed to create {}", old_root_dir);
    return false;
  }

  // Ensure .oldroot is empty before pivot_root.
  DIR* d = opendir(old_root_dir.c_str());
  if (!d) {
    log_.error("Failed to open {}: errno={}", old_root_dir, errno);
    return false;
  }

  bool empty = true;
  while (const struct dirent* e = readdir(d)) {
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
      empty = false;
      break;
    }
  }
  closedir(d);

  if (!empty) {
    log_.error("{} is not empty, cannot pivot_root", old_root_dir);
    return false;
  }

  return true;
}

bool RootManager::MountEssentialFilesystems() {
  log_.info("Moving essential filesystems to new root");

  const std::array moves{
      std::pair{"/dev", std::format("{}/dev", new_root_path_)},
      std::pair{"/proc", std::format("{}/proc", new_root_path_)},
      std::pair{"/sys", std::format("{}/sys", new_root_path_)},
  };

  for (const auto& [src, dst] : moves) {
    if (!mount_manager_->EnsureDirectory(dst)) {
      log_.error("Failed to create {}", dst);
      return false;
    }

    log_.info("MS_MOVE {} -> {}", src, dst);
    if (mount(src, dst.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
      log_.error(
          "Failed to move {}: errno={} ({})", src, errno, strerror(errno));
      DumpMountInfo("MS_MOVE failed");
      return false;
    }
  }

  return true;
}

bool RootManager::SwitchRoot() {
  // What BusyBox switch_root did, in-process: free the initramfs, move the
  // new root onto /, chroot into it and exec aember from it as PID 1.
  log_.info("Switching root to {}", new_root_path_);

  if (::chdir(new_root_path_.c_str()) != 0) {
    log_.error("chdir {}: {}", new_root_path_, strerror(errno));
    return false;
  }

  // The initramfs stays in RAM until its files are deleted. Only delete if
  // / really is the initramfs (ramfs/tmpfs), never on a disk.
  struct statfs root_fs {};
  if (::statfs("/", &root_fs) == 0 &&
      (root_fs.f_type == RAMFS_MAGIC || root_fs.f_type == TMPFS_MAGIC)) {
    struct stat root_st {};
    if (::stat("/", &root_st) == 0) {
      const int dir = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir >= 0) {
        DeleteInitramfs(dir, root_st.st_dev);
        ::close(dir);
      }
    }
  } else {
    log_.warn("/ is not an initramfs; leaving its contents alone");
  }

  if (::mount(".", "/", nullptr, MS_MOVE, nullptr) != 0) {
    log_.error("MS_MOVE {} -> /: {}", new_root_path_, strerror(errno));
    return false;
  }
  if (::chroot(".") != 0 || ::chdir("/") != 0) {
    log_.error("chroot into new root: {}", strerror(errno));
    return false;
  }

  std::array<char*, 3> args{
      const_cast<char*>(kRootInit), const_cast<char*>("--root"), nullptr};
  log_.info("Executing {} --root", kRootInit);
  aember::utils::logging::flush();
  ::execv(args[0], args.data());

  // execv only returns on failure.
  log_.error("exec {}: {}", kRootInit, strerror(errno));
  return false;
}

void RootManager::DeleteInitramfs(int dir_fd, dev_t root_dev) {
  // Recursive delete that stays on the initramfs: directories on another
  // device (the new root, /dev, /proc, /sys) are mount points and skipped.
  DIR* dir = ::fdopendir(::dup(dir_fd));
  if (dir == nullptr) return;
  while (const dirent* e = ::readdir(dir)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
      continue;
    }
    struct stat st {};
    if (::fstatat(dir_fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (st.st_dev != root_dev) continue;  // another filesystem
    if (S_ISDIR(st.st_mode)) {
      const int sub =
          ::openat(dir_fd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (sub >= 0) {
        DeleteInitramfs(sub, root_dev);
        ::close(sub);
      }
      ::unlinkat(dir_fd, e->d_name, AT_REMOVEDIR);
    } else {
      ::unlinkat(dir_fd, e->d_name, 0);
    }
  }
  ::closedir(dir);
}

bool RootManager::UnmountOldRoot() {
  log_.info("Unmounting old root");

  if (umount2("/.oldroot", MNT_DETACH) != 0) {
    log_.warn(
        "Failed to unmount old root: errno={} ({})", errno, strerror(errno));
    return false;
  }

  if (rmdir("/.oldroot") != 0) {
    log_.debug("Failed to remove /.oldroot dir: errno={}", errno);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Device resolution
// ---------------------------------------------------------------------------

std::string RootManager::ResolveDevice(std::string_view device) {
  if (device.starts_with("UUID=")) {
    return FindDeviceByUUID(device.substr(5));
  }
  if (device.starts_with("LABEL=")) {
    return FindDeviceByLabel(device.substr(6));
  }
  return std::string{device};
}

std::string RootManager::FindDeviceByUUID(std::string_view uuid) {
  log_.debug("Searching device by UUID={}", uuid);
  return ResolveSymlink(std::format("/dev/disk/by-uuid/{}", uuid));
}

std::string RootManager::FindDeviceByLabel(std::string_view label) {
  log_.debug("Searching device by LABEL={}", label);
  return ResolveSymlink(std::format("/dev/disk/by-label/{}", label));
}

std::string RootManager::ResolveSymlink(std::string_view path) {
  const std::string path_str{path};

  struct stat st {};
  if (lstat(path_str.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) { return {}; }

  char link[PATH_MAX];
  const ssize_t len = readlink(path_str.c_str(), link, sizeof(link) - 1);
  if (len <= 0) { return {}; }
  link[len] = '\0';

  char real[PATH_MAX];
  if (realpath(path_str.c_str(), real)) { return real; }

  return {};
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

bool RootManager::IsInInitramfs() {
  return !std::ifstream{"/etc/aember/services.json"}.good();
}

void RootManager::DumpMountInfo(std::string_view reason) {
  log_.error("Dumping mountinfo due to: {}", reason);

  std::ifstream f{"/proc/self/mountinfo"};
  std::string line;
  while (std::getline(f, line)) { log_.error("mountinfo: {}", line); }
}

}  // namespace aember::root_manager
