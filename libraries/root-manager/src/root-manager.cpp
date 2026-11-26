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

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <thread>

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
    log_.error("Failed to set MS_PRIVATE: errno={} ({})",
               errno,
               std::system_category().message(errno));
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
    log_.error("Failed to mount real root: errno={} ({})",
               errno,
               std::system_category().message(errno));
    DumpMountInfo("mount real root failed");
    return false;
  }

  return true;
}

bool RootManager::PivotToNewRoot() {
  log_.info("Pivoting to new root: {}", new_root_path_);

  if (!PrepareNewRoot()) { return false; }
  if (!MountEssentialFilesystems()) { return false; }
  // Execs aember from the new root; only returns if that failed. (There is
  // no old root to unmount: MS_MOVE replaces it, unlike pivot_root.)
  SwitchRoot();
  return false;
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

  return true;
}

bool RootManager::MountEssentialFilesystems() {
  log_.info("Moving essential filesystems to new root");

  const std::array moves{
      std::pair{"/dev", fmt::format("{}/dev", new_root_path_)},
      std::pair{"/proc", fmt::format("{}/proc", new_root_path_)},
      std::pair{"/sys", fmt::format("{}/sys", new_root_path_)},
  };

  for (const auto& [src, dst] : moves) {
    if (!mount_manager_->EnsureDirectory(dst)) {
      log_.error("Failed to create {}", dst);
      return false;
    }

    log_.info("MS_MOVE {} -> {}", src, dst);
    if (mount(src, dst.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
      log_.error("Failed to move {}: errno={} ({})",
                 src,
                 errno,
                 std::system_category().message(errno));
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
    log_.error(
        "chdir {}: {}", new_root_path_, std::system_category().message(errno));
    return false;
  }

  // The initramfs stays in RAM until its files are deleted. Only delete if
  // / really is the initramfs (ramfs/tmpfs), never on a disk.
  struct statfs root_fs {};
  // f_type is long on 64-bit and int on 32-bit ARM, the magics are 32-bit
  // unsigned (RAMFS_MAGIC doesn't fit an int): compare as uint32_t.
  const auto fs_type = [&] {
    return static_cast<std::uint32_t>(root_fs.f_type);
  };
  if (::statfs("/", &root_fs) == 0 &&
      (fs_type() == RAMFS_MAGIC || fs_type() == TMPFS_MAGIC)) {
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
    log_.error("MS_MOVE {} -> /: {}",
               new_root_path_,
               std::system_category().message(errno));
    return false;
  }
  if (::chroot(".") != 0 || ::chdir("/") != 0) {
    log_.error("chroot into new root: {}",
               std::system_category().message(errno));
    return false;
  }

  std::array<char*, 3> args{
      const_cast<char*>(kRootInit), const_cast<char*>("--root"), nullptr};
  log_.info("Executing {} --root", kRootInit);
  aember::utils::logging::flush();
  ::execv(args[0], args.data());

  // execv only returns on failure.
  log_.error("exec {}: {}", kRootInit, std::system_category().message(errno));
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

// ---------------------------------------------------------------------------
// Device resolution
// ---------------------------------------------------------------------------

std::string RootManager::ResolveDevice(std::string_view device) {
  // rootwait: the root device (an SD card on a Raspberry Pi) shows up
  // asynchronously after the kernel started init. Poll for it.
  const auto deadline = std::chrono::steady_clock::now() + root_wait_;
  const bool by_uuid = device.starts_with("UUID=");
  const bool by_label = device.starts_with("LABEL=");
  bool logged = false;
  while (true) {
    std::string found;
    if (by_uuid) {
      found = FindBlockDevice("UUID", device.substr(5));
    } else if (by_label) {
      found = FindBlockDevice("LABEL", device.substr(6));
    } else if (::access(std::string{device}.c_str(), F_OK) == 0) {
      found = device;
    }
    if (!found.empty()) { return found; }
    if (std::chrono::steady_clock::now() >= deadline) { return {}; }
    if (!logged) {
      log_.info("Waiting for root device {}", device);
      logged = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
  }
}

std::optional<RootManager::Ext4Id> RootManager::ReadExt4Id(
    const std::string& path) {
  std::ifstream dev{path, std::ios::binary};
  if (!dev) { return std::nullopt; }
  // The superblock starts at byte 1024: s_magic at 0x38, s_uuid at 0x68,
  // s_volume_name at 0x78 (16 bytes each, label NUL-padded).
  std::array<unsigned char, 1024> sb{};
  dev.seekg(1024);
  if (!dev.read(reinterpret_cast<char*>(sb.data()), sb.size())) {
    return std::nullopt;
  }
  if (sb[0x38] != 0x53 || sb[0x39] != 0xEF) { return std::nullopt; }

  Ext4Id id;
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) { id.uuid += '-'; }
    id.uuid += fmt::format("{:02x}", sb[0x68 + i]);
  }
  const auto* label = reinterpret_cast<const char*>(&sb[0x78]);
  id.label.assign(label, strnlen(label, 16));
  return id;
}

std::string RootManager::FindBlockDevice(std::string_view key,
                                         std::string_view value,
                                         const std::string& sys_block,
                                         const std::string& dev_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator{sys_block, ec}) {
    const auto path = dev_dir + "/" + entry.path().filename().string();
    const auto id = ReadExt4Id(path);
    if (!id) { continue; }
    if ((key == "LABEL" && id->label == value) ||
        (key == "UUID" && id->uuid == value)) {
      return path;
    }
  }
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
