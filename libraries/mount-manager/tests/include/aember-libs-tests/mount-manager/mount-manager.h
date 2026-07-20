#pragma once

#include <aember-libs/mount-manager/mount-manager.h>

#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aember_test::mount_manager {

class MountManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Ensure /proc is mounted (needed for IsMounted checks)
    if (!PathExists("/proc/mounts")) {
      GTEST_SKIP() << "/proc is not mounted - required for mount tests";
    }

    manager_ = std::make_unique<aember::mount_manager::MountManager>();

    // Try multiple locations for test directory
    std::vector<std::string> candidate_dirs = {
        "/run/mount_test_" + std::to_string(getpid()),
        "/tmp/mount_test_" + std::to_string(getpid()),
        "/var/tmp/mount_test_" + std::to_string(getpid()),
        "/mnt/mount_test_" + std::to_string(getpid())};

    bool dir_created = false;
    for (const auto& dir : candidate_dirs) {
      if (mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) {
        test_dir_ = dir;
        dir_created = true;
        break;
      }
    }

    if (!dir_created) {
      GTEST_SKIP() << "Cannot create test directory in any location";
    }

    test_mount_point_ = test_dir_ + "/mnt";
    test_mount_source_ = test_dir_ + "/src";
  }

  void TearDown() override {
    // Clean up any test mounts
    if (!test_mount_point_.empty()) {
      umount2(test_mount_point_.c_str(), MNT_DETACH);
    }

    // Remove test directories
    if (!test_mount_point_.empty()) { rmdir(test_mount_point_.c_str()); }
    if (!test_mount_source_.empty()) { rmdir(test_mount_source_.c_str()); }
    if (!test_dir_.empty()) {
      // Remove any leftover subdirectories
      system(("rm -rf " + test_dir_).c_str());
    }

    manager_.reset();
  }

  // Helper: Check if a path exists
  bool PathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
  }

  // Helper: Check if a path is a directory
  bool IsDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
  }

  // Helper: Create a simple test mount (tmpfs)
  bool CreateTestMount(const std::string& target) {
    if (mkdir(target.c_str(), 0755) != 0 && errno != EEXIST) { return false; }
    return mount("tmpfs", target.c_str(), "tmpfs", 0, "size=1M") == 0;
  }

  // Helper: Check if mounted via /proc/mounts
  bool IsActuallyMounted(const std::string& target) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
      if (line.find(target) != std::string::npos) { return true; }
    }
    return false;
  }

  std::unique_ptr<aember::mount_manager::MountManager> manager_;
  std::string test_dir_;
  std::string test_mount_point_;
  std::string test_mount_source_;
};

}  // namespace aember_test::mount_manager
