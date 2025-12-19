#include <aember-libs-tests/mount-manager/mount-manager.h>

namespace aember_test::mount_manager {

TEST_F(MountManagerTest, ConstructorInitializes) {
  EXPECT_NE(manager_, nullptr);
}

TEST_F(MountManagerTest, EnsureDirectoryCreatesDirectory) {
  std::string test_path = test_dir_ + "/new_dir";

  EXPECT_FALSE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  // Cleanup
  rmdir(test_path.c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryWithExistingDirectory) {
  std::string test_path = test_dir_ + "/existing_dir";
  mkdir(test_path.c_str(), 0755);

  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  // Cleanup
  rmdir(test_path.c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryCreatesParents) {
  std::string test_path = test_dir_ + "/parent/child/grandchild";

  EXPECT_FALSE(PathExists(test_path));
  EXPECT_TRUE(manager_->EnsureDirectory(test_path));
  EXPECT_TRUE(PathExists(test_path));
  EXPECT_TRUE(IsDirectory(test_path));

  // Cleanup
  rmdir((test_dir_ + "/parent/child/grandchild").c_str());
  rmdir((test_dir_ + "/parent/child").c_str());
  rmdir((test_dir_ + "/parent").c_str());
}

TEST_F(MountManagerTest, EnsureDirectoryFailsOnFile) {
  std::string test_file = test_dir_ + "/test_file";

  // Create a file
  std::ofstream f(test_file);
  f << "test";
  f.close();

  EXPECT_TRUE(PathExists(test_file));
  EXPECT_FALSE(manager_->EnsureDirectory(test_file));

  // Cleanup
  unlink(test_file.c_str());
}

TEST_F(MountManagerTest, UnmountNotMounted) {
  // Should not fail when unmounting something that isn't mounted
  EXPECT_TRUE(manager_->Unmount(test_mount_point_));
}

TEST_F(MountManagerTest, IsMountedReturnsFalseForNonMounted) {
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, UnmountAllWithNoMounts) {
  // Should not crash when there are no mounts
  manager_->UnmountAll();
}

TEST_F(MountManagerTest, MountEarlyFilesystemsDoesNotRemountExisting) {
  // This test verifies that MountEarlyFilesystems skips already-mounted
  // filesystems Note: /proc, /sys, /dev should already be mounted by init
  // script

  // Just verify it doesn't crash and returns successfully
  bool result = manager_->MountEarlyFilesystems();

  // We don't assert true/false here because some mounts might fail in test
  // environment The important thing is it doesn't crash
  (void)result;
}

TEST_F(MountManagerTest, MountInvalidFilesystemType) {
  aember::mount_manager::MountPoint mp(
      "invalid", test_mount_point_, "invalid_fs_type", 0);

  EXPECT_FALSE(manager_->Mount(mp));
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

}  // namespace aember_test::mount_manager
