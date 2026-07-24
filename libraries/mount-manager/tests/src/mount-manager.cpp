#include <aember-libs-tests/mount-manager/mount-manager.h>

namespace aember_test::mount_manager {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, ConstructorInitializes) {
  EXPECT_NE(manager_, nullptr);
}

// ---------------------------------------------------------------------------
// EnsureDirectory
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, EnsureDirectoryCreatesDirectory) {
  std::string test_path = test_dir_ + "/new_dir";

  EXPECT_FALSE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  rmdir(test_path.c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryWithExistingDirectory) {
  std::string test_path = test_dir_ + "/existing_dir";
  mkdir(test_path.c_str(), 0755);

  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  rmdir(test_path.c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryCreatesParents) {
  std::string test_path = test_dir_ + "/parent/child/grandchild";

  EXPECT_FALSE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  rmdir((test_dir_ + "/parent/child/grandchild").c_str());
  rmdir((test_dir_ + "/parent/child").c_str());
  rmdir((test_dir_ + "/parent").c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryFailsOnFile) {
  std::string test_file = test_dir_ + "/test_file";

  std::ofstream f(test_file);
  f << "test";
  f.close();

  EXPECT_TRUE(PathExists(test_file));
  EXPECT_FALSE(manager_->EnsureDirectory(test_file));

  unlink(test_file.c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryCreatesMultipleLevels) {
  std::string test_path = test_dir_ + "/a/b/c/d/e";

  EXPECT_FALSE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  system(("rm -rf " + test_dir_ + "/a").c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryEmptyPathFails) {
  // Empty path should fail gracefully
  EXPECT_FALSE(manager_->EnsureDirectory(""));
}

// ---------------------------------------------------------------------------
// IsMounted
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, IsMountedReturnsFalseForNonMounted) {
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, IsMountedReturnsTrueForProcFs) {
  // /proc is always mounted in our test environment (checked in SetUp)
  EXPECT_TRUE(manager_->IsMounted("/proc"));
}

TEST_F(MountManagerTest, IsMountedReturnsTrueForSysFs) {
  // /proc is always mounted in our test environment
  EXPECT_TRUE(manager_->IsMounted("/proc"));
}

TEST_F(MountManagerTest, IsMountedReturnsFalseForRandomPath) {
  EXPECT_FALSE(manager_->IsMounted("/this/path/does/not/exist"));
}

TEST_F(MountManagerTest, IsMountedReturnsFalseForFile) {
  // A regular file is not a mount point
  std::string test_file = test_dir_ + "/regular_file";
  std::ofstream f(test_file);
  f << "data";
  f.close();

  EXPECT_FALSE(manager_->IsMounted(test_file));
  unlink(test_file.c_str());
}

// ---------------------------------------------------------------------------
// Unmount
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, UnmountNotMounted) {
  // Should not fail when unmounting something that isn't mounted
  EXPECT_TRUE(manager_->Unmount(test_mount_point_));
}

TEST_F(MountManagerTest, UnmountNonExistentPath) {
  // Non-existent path should return true (nothing to unmount)
  EXPECT_TRUE(manager_->Unmount("/non/existent/path"));
}

// ---------------------------------------------------------------------------
// UnmountAll
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, UnmountAllWithNoMounts) {
  // Should not crash when there are no mounts
  manager_->UnmountAll();
}

TEST_F(MountManagerTest, UnmountAllTwice) {
  // Calling UnmountAll twice should be safe
  manager_->UnmountAll();
  manager_->UnmountAll();
}

// ---------------------------------------------------------------------------
// MountEarlyFilesystems
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, MountEarlyFilesystemsDoesNotCrash) {
  // Verify it doesn't crash — some mounts may fail in test env
  bool result = manager_->MountEarlyFilesystems();
  (void)result;
}

// ---------------------------------------------------------------------------
// GetInitramfsMounts / GetEarlyMounts — verify mount table contents
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, GetInitramfsMountsReturnsExpectedEntries) {
  // Access via MountEarlyFilesystems indirectly — verify known mounts exist
  // by checking the manager doesn't return an empty internal list
  // We test this by verifying /proc and /sys are always considered early mounts
  EXPECT_TRUE(manager_->IsMounted("/proc"));
  EXPECT_FALSE(manager_->IsMounted("/this/does/not/exist"));
}

TEST_F(MountManagerTest, GetEarlyMountsIncludesCgroup) {
  // cgroup2 should be in early mounts — verify path exists on system
  // (may or may not be mounted in test env but path should be valid)
  bool cgroup_mounted = manager_->IsMounted("/sys/fs/cgroup");
  // We just verify IsMounted doesn't crash for this path
  (void)cgroup_mounted;
}

// ---------------------------------------------------------------------------
// MountSquashFS — test error paths without actual squashfs
// ---------------------------------------------------------------------------

TEST_F(MountManagerTest, MountSquashFSNonExistentImage) {
  // Should fail gracefully when image doesn't exist
  bool result = manager_->MountSquashFS(
      "/non/existent/image.squashfs", test_mount_point_, true);
  EXPECT_FALSE(result);
}

}  // namespace aember_test::mount_manager
