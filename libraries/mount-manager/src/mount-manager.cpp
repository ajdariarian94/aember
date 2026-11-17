/**
 * @file mount-manager.cpp
 * @author Arian Ajdari
 * @brief MountManager implementation.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/mount-manager/mount-manager.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>

#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aember::mount_manager {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Ctor
// ---------------------------------------------------------------------------

MountManager::MountManager() {
  log_.info("MountManager initialized");
}

// ---------------------------------------------------------------------------
// Bulk mount phases
// ---------------------------------------------------------------------------

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

bool MountManager::MountEarlyFilesystems() {
  log_.info("Mounting early boot filesystems...");

  const auto mounts = GetEarlyMounts();

  const bool all_ok = std::ranges::all_of(mounts, [this](const auto& mp) {
    if (!Mount(mp)) {
      log_.warn("Failed to mount {}, continuing anyway", mp.target);
      return false;
    }
    return true;
  });

  if (all_ok) {
    log_.info("All early filesystems mounted successfully");
  } else {
    log_.warn("Some early filesystems failed to mount");
  }

  return all_ok;
}

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------

bool MountManager::Mount(const MountPoint& mp) {
  if (IsMounted(mp.target)) {
    log_.debug("'{}' already mounted, skipping", mp.target);
    return true;
  }

  if (!EnsureDirectory(mp.target)) { return false; }

  log_.info("Mounting {} on {} (type: {})", mp.source, mp.target, mp.fstype);

  const char* data = mp.data.empty() ? nullptr : mp.data.c_str();

  if (::mount(mp.source.c_str(),
              mp.target.c_str(),
              mp.fstype.c_str(),
              mp.flags,
              data) != 0) {
    log_.error(
        "Failed to mount {} on {}: {}", mp.source, mp.target, strerror(errno));
    return false;
  }

  mounted_targets_.push_back(mp.target);
  log_.info("Mounted {} on {}", mp.source, mp.target);
  return true;
}

// ---------------------------------------------------------------------------
// Loop devices (what `mount -o loop` / BusyBox did for us)
// ---------------------------------------------------------------------------

MountManager::LoopDevice::~LoopDevice() {
  if (fd >= 0) ::close(fd);
}

std::expected<MountManager::LoopDevice, std::string> MountManager::AttachLoop(
    std::string_view image) {
  const int file = ::open(std::string{image}.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) {
    return std::unexpected(std::format("open {}: {}", image, strerror(errno)));
  }
  struct FdCloser {
    int fd;
    ~FdCloser() {
      if (fd >= 0) ::close(fd);
    }
  } file_closer{file};

  const int control = ::open("/dev/loop-control", O_RDWR | O_CLOEXEC);
  if (control < 0) {
    return std::unexpected(
        std::format("open /dev/loop-control: {}", strerror(errno)));
  }
  FdCloser control_closer{control};

  // Another process can grab the free device between GET_FREE and
  // CONFIGURE (EBUSY): just ask again.
  for (int attempt = 0; attempt < 8; ++attempt) {
    const int n = ::ioctl(control, LOOP_CTL_GET_FREE);
    if (n < 0) {
      return std::unexpected(
          std::format("LOOP_CTL_GET_FREE: {}", strerror(errno)));
    }
    const auto device = std::format("/dev/loop{}", n);
    const int loop = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (loop < 0) {
      return std::unexpected(
          std::format("open {}: {}", device, strerror(errno)));
    }
    LoopDevice handle{device, loop};  // keeps the fd open for the caller

    loop_config config{};
    config.fd = static_cast<__u32>(file);
    config.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
    std::snprintf(reinterpret_cast<char*>(config.info.lo_file_name),
                  LO_NAME_SIZE,
                  "%.*s",
                  static_cast<int>(image.size()),
                  image.data());
    if (::ioctl(loop, LOOP_CONFIGURE, &config) == 0) return handle;
    if (errno != EBUSY) {
      return std::unexpected(
          std::format("LOOP_CONFIGURE {}: {}", device, strerror(errno)));
    }
  }
  return std::unexpected("no free loop device");
}

bool MountManager::MountSquashFS(std::string_view image,
                                 std::string_view target, bool /*read_only*/) {
  if (IsMounted(target)) {
    log_.debug("'{}' already mounted, skipping", target);
    return true;
  }

  if (!EnsureDirectory(target)) { return false; }

  const auto loop = AttachLoop(image);
  if (!loop) {
    log_.error("Cannot attach {} to a loop device: {}", image, loop.error());
    return false;
  }

  // The loop device is set up with autoclear: it detaches by itself when
  // this mount goes away, or when `loop` closes its fd if the mount fails.
  if (::mount(loop->path.c_str(),
              std::string{target}.c_str(),
              "squashfs",
              MS_RDONLY,
              nullptr) != 0) {
    log_.error("mount {} ({}) on {} failed: {}",
               image,
               loop->path,
               target,
               strerror(errno));
    return false;
  }

  mounted_targets_.emplace_back(target);
  log_.info("Mounted {} on {} via {}", image, target, loop->path);

  // Overlay a writable tmpfs on <target>/tmp.
  const auto tmp_path = std::format("{}/tmp", target);
  if (!EnsureDirectory(tmp_path)) {
    log_.warn("Could not ensure tmp directory at {}", tmp_path);
    return true;  // squashfs itself succeeded
  }

  if (::mount("tmpfs", tmp_path.c_str(), "tmpfs", 0, nullptr) != 0) {
    log_.warn("Failed to mount tmpfs on {}: {}", tmp_path, strerror(errno));
  } else {
    mounted_targets_.push_back(tmp_path);
    log_.info("Mounted tmpfs on {}", tmp_path);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Unmount
// ---------------------------------------------------------------------------

bool MountManager::Unmount(std::string_view target, bool force) {
  log_.info("Unmounting {}{}", target, force ? " (forced)" : "");

  const int flags = force ? MNT_FORCE : 0;
  const std::string target_str{target};

  if (::umount2(target_str.c_str(), flags) != 0) {
    if (errno == EINVAL || errno == ENOENT) {
      log_.debug("{} was not mounted", target);
      return true;
    }
    log_.error("Failed to unmount {}: {}", target, strerror(errno));
    return false;
  }

  std::erase(mounted_targets_, target_str);

  log_.info("Unmounted {}", target);
  return true;
}

void MountManager::UnmountAll(bool force) {
  log_.info("Unmounting all tracked filesystems...");

  for (const auto& target : mounted_targets_ | std::views::reverse) {
    Unmount(target, force);
  }

  mounted_targets_.clear();
}

// ---------------------------------------------------------------------------
// IsMounted
// ---------------------------------------------------------------------------

bool MountManager::IsMounted(std::string_view target) const {
  std::ifstream proc_mounts{"/proc/mounts"};
  if (!proc_mounts.is_open()) {
    log_.warn("Could not open /proc/mounts");
    return false;
  }

  // /proc/mounts format: device mountpoint fstype options dump pass
  // Read the second token (mountpoint) from each line and compare.
  std::string line;
  while (std::getline(proc_mounts, line)) {
    std::istringstream ss{line};
    std::string device, mountpoint;
    ss >> device >> mountpoint;

    // Unescape \040 (space encoded in /proc/mounts).
    for (std::size_t pos = 0;
         (pos = mountpoint.find("\\040", pos)) != std::string::npos;) {
      mountpoint.replace(pos, 4, " ");
      pos += 1;
    }

    if (mountpoint == target) { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------
// EnsureDirectory
// ---------------------------------------------------------------------------

bool MountManager::EnsureDirectory(std::string_view path, mode_t mode) {
  const fs::path p{path};

  std::error_code ec;
  const auto status = fs::status(p, ec);

  if (!ec && fs::is_directory(status)) { return true; }

  if (!ec && fs::exists(status)) {
    log_.error("Path '{}' exists but is not a directory", path);
    return false;
  }

  if (!fs::create_directories(p, ec) && ec) {
    log_.error("Failed to create directory '{}': {}", path, ec.message());
    return false;
  }

  // std::filesystem::create_directories uses umask; set explicit permissions.
  if (::chmod(std::string{path}.c_str(), mode) != 0) {
    log_.warn("chmod failed for '{}': {}", path, strerror(errno));
  }

  log_.debug("Created directory '{}'", path);
  return true;
}

// ---------------------------------------------------------------------------
// Mount tables
// ---------------------------------------------------------------------------

std::vector<MountManager::MountPoint> MountManager::GetInitramfsMounts() const {
  return {
      {.source = "proc",
       .target = "/proc",
       .fstype = "proc",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV},
      {.source = "sysfs",
       .target = "/sys",
       .fstype = "sysfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV},
      {.source = "devtmpfs",
       .target = "/dev",
       .fstype = "devtmpfs",
       .flags = MS_NOSUID},
      {.source = "tmpfs",
       .target = "/tmp",
       .fstype = "tmpfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV,
       .data = "mode=1777"},
      {.source = "tmpfs",
       .target = "/mnt",
       .fstype = "tmpfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV},
  };
}

std::vector<MountManager::MountPoint> MountManager::GetEarlyMounts() const {
  return {
      {.source = "devpts",
       .target = "/dev/pts",
       .fstype = "devpts",
       .flags = MS_NOEXEC | MS_NOSUID,
       .data = "mode=0620,gid=5,ptmxmode=0666"},
      {.source = "tmpfs",
       .target = "/dev/shm",
       .fstype = "tmpfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV,
       .data = "mode=1777"},
      {.source = "tmpfs",
       .target = "/run",
       .fstype = "tmpfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV,
       .data = "mode=0755,size=10%"},
      {.source = "tmpfs",
       .target = "/tmp",
       .fstype = "tmpfs",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV,
       .data = "mode=1777"},
      {.source = "cgroup2",
       .target = "/sys/fs/cgroup",
       .fstype = "cgroup2",
       .flags = MS_NOEXEC | MS_NOSUID | MS_NODEV,
       .data = "nsdelegate"},
  };
}

}  // namespace aember::mount_manager
