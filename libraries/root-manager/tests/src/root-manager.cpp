/**
 * @file root-manager.cpp
 * @author Arian Ajdari
 * @brief RootManager tests: freeing the initramfs (the part of switch_root
 *        that can run outside PID 1) and the pivot's error paths.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/root-manager/root-manager.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// As root (the QEMU VM, `docker exec -u root`), this test binary gets its
// own mount namespace before any test runs (static init: still single
// threaded), so nothing the tests mount leaks into the system. Mounting
// tests skip if that isn't possible.
const bool kPrivateMounts = [] {
  if (::geteuid() != 0) return false;
  if (::unshare(CLONE_NEWNS) != 0) return false;
  return ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0;
}();

}  // namespace

/// Skip a test that mounts unless it runs isolated (or as non-root, where
/// mounting fails harmlessly).
#define AEMBER_REQUIRE_PRIVATE_MOUNTS()                         \
  do {                                                          \
    if (::geteuid() == 0 && !kPrivateMounts)                    \
      GTEST_SKIP() << "root without a private mount namespace"; \
  } while (0)

namespace {

namespace fs = std::filesystem;
using aember::root_manager::RootManager;

class DeleteInitramfsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-rm-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_ / "usr/bin");
    fs::create_directories(dir_ / "etc/aember");
    fs::create_directories(dir_ / "empty");
    std::ofstream{dir_ / "init"} << "x";
    std::ofstream{dir_ / "usr/bin/aember"} << "binary";
    std::ofstream{dir_ / "etc/aember/modules.json"} << "{}";
    fs::create_symlink("usr/bin/aember", dir_ / "linit");
    // A symlink pointing outside must be removed, never followed.
    outside_ = fs::temp_directory_path() /
               ("aember-rm-outside-" + std::to_string(::getpid()));
    std::ofstream{outside_} << "keep me";
    fs::create_symlink(outside_, dir_ / "escape");
  }
  void TearDown() override {
    if (::geteuid() == 0) ::umount2((dir_ / "mnt").c_str(), MNT_DETACH);
    fs::remove_all(dir_);
    fs::remove(outside_);
  }

  void Delete() {
    struct stat st {};
    ASSERT_EQ(::stat(dir_.c_str(), &st), 0);
    const int fd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
    ASSERT_GE(fd, 0);
    RootManager::DeleteInitramfs(fd, st.st_dev);
    ::close(fd);
  }

  fs::path dir_;
  fs::path outside_;
};

TEST_F(DeleteInitramfsTest, DeletesEverythingOnTheSameFilesystem) {
  Delete();
  EXPECT_TRUE(fs::exists(dir_));  // the top directory itself stays
  EXPECT_TRUE(fs::is_empty(dir_));
  EXPECT_TRUE(fs::exists(outside_)) << "followed a symlink out of the tree";
}

// As root (inside the VM): a mount point inside the tree (like the new
// root, /dev, /proc during switch_root) is another filesystem and kept.
TEST_F(DeleteInitramfsTest, KeepsMountPointsAsRoot) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  AEMBER_REQUIRE_PRIVATE_MOUNTS();
  fs::create_directories(dir_ / "mnt");
  ASSERT_EQ(::mount("tmpfs", (dir_ / "mnt").c_str(), "tmpfs", 0, nullptr), 0);
  std::ofstream{dir_ / "mnt/on-other-fs"} << "keep";
  Delete();
  EXPECT_TRUE(fs::exists(dir_ / "mnt/on-other-fs"));
  EXPECT_FALSE(fs::exists(dir_ / "usr"));
  EXPECT_FALSE(fs::exists(dir_ / "init"));
}

TEST(RootManagerTest, PivotWithoutRootDeviceFails) {
  RootManager rm{std::make_shared<aember::mount_manager::MountManager>()};
  aember::utils::root::RootConfig cfg;  // no device
  EXPECT_FALSE(rm.MountRealRoot(cfg));
  cfg.device = "/dev/aember-no-such-disk";
  cfg.fstype = "";
  EXPECT_FALSE(rm.MountRealRoot(cfg));
}

}  // namespace
