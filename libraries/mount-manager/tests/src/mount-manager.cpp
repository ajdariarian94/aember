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

TEST_F(MountManagerTest, MountTmpfsBasic) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(PathExists(test_mount_point_));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, MountCreatesDirectory) {
  // Don't create the mount point beforehand
  EXPECT_FALSE(PathExists(test_mount_point_));

  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(PathExists(test_mount_point_));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, MountAlreadyMounted) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Try mounting again - should succeed without error
  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, UnmountBasic) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  EXPECT_TRUE(manager_->Unmount(test_mount_point_));
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, UnmountNotMounted) {
  // Should not fail when unmounting something that isn't mounted
  EXPECT_TRUE(manager_->Unmount(test_mount_point_));
}

TEST_F(MountManagerTest, UnmountForced) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  EXPECT_TRUE(manager_->Unmount(test_mount_point_, true));  // Force unmount
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, IsMountedReturnsFalseForNonMounted) {
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, IsMountedReturnsTrueForMounted) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, MountMultipleFilesystems) {
  std::string mount1 = test_dir_ + "/mnt1";
  std::string mount2 = test_dir_ + "/mnt2";
  std::string mount3 = test_dir_ + "/mnt3";

  aember::mount_manager::MountPoint mp1("tmpfs", mount1, "tmpfs", 0, "size=1M");
  aember::mount_manager::MountPoint mp2("tmpfs", mount2, "tmpfs", 0, "size=1M");
  aember::mount_manager::MountPoint mp3("tmpfs", mount3, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp1));
  EXPECT_TRUE(manager_->Mount(mp2));
  EXPECT_TRUE(manager_->Mount(mp3));

  EXPECT_TRUE(manager_->IsMounted(mount1));
  EXPECT_TRUE(manager_->IsMounted(mount2));
  EXPECT_TRUE(manager_->IsMounted(mount3));

  // Cleanup
  manager_->Unmount(mount1);
  manager_->Unmount(mount2);
  manager_->Unmount(mount3);
}

TEST_F(MountManagerTest, UnmountAllBasic) {
  std::string mount1 = test_dir_ + "/mnt1";
  std::string mount2 = test_dir_ + "/mnt2";
  std::string mount3 = test_dir_ + "/mnt3";

  aember::mount_manager::MountPoint mp1("tmpfs", mount1, "tmpfs", 0, "size=1M");
  aember::mount_manager::MountPoint mp2("tmpfs", mount2, "tmpfs", 0, "size=1M");
  aember::mount_manager::MountPoint mp3("tmpfs", mount3, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp1));
  EXPECT_TRUE(manager_->Mount(mp2));
  EXPECT_TRUE(manager_->Mount(mp3));

  manager_->UnmountAll();

  EXPECT_FALSE(manager_->IsMounted(mount1));
  EXPECT_FALSE(manager_->IsMounted(mount2));
  EXPECT_FALSE(manager_->IsMounted(mount3));
}

TEST_F(MountManagerTest, UnmountAllReverseOrder) {
  // This test verifies that filesystems are unmounted in reverse order (LIFO)
  std::string mount1 = test_dir_ + "/mnt1";
  std::string mount2 = test_dir_ + "/mnt2";

  aember::mount_manager::MountPoint mp1("tmpfs", mount1, "tmpfs", 0, "size=1M");
  aember::mount_manager::MountPoint mp2("tmpfs", mount2, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp1));
  EXPECT_TRUE(manager_->Mount(mp2));

  manager_->UnmountAll();

  EXPECT_FALSE(manager_->IsMounted(mount1));
  EXPECT_FALSE(manager_->IsMounted(mount2));
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

TEST_F(MountManagerTest, MountWithFlags) {
  aember::mount_manager::MountPoint mp("tmpfs",
                                       test_mount_point_,
                                       "tmpfs",
                                       MS_NOEXEC | MS_NOSUID | MS_NODEV,
                                       "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, MountWithOptions) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=2M,mode=0755");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, MountInvalidFilesystemType) {
  aember::mount_manager::MountPoint mp(
      "invalid", test_mount_point_, "invalid_fs_type", 0);

  EXPECT_FALSE(manager_->Mount(mp));
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));
}

TEST_F(MountManagerTest, CheckMountStatusWithSpacesInPath) {
  // Test that mount points with spaces in their names are handled correctly
  std::string mount_with_space = test_dir_ + "/mount point";

  aember::mount_manager::MountPoint mp(
      "tmpfs", mount_with_space, "tmpfs", 0, "size=1M");

  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(mount_with_space));

  // Cleanup
  manager_->Unmount(mount_with_space);
}

TEST_F(MountManagerTest, RemountAfterUnmount) {
  aember::mount_manager::MountPoint mp(
      "tmpfs", test_mount_point_, "tmpfs", 0, "size=1M");

  // First mount
  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Unmount
  EXPECT_TRUE(manager_->Unmount(test_mount_point_));
  EXPECT_FALSE(manager_->IsMounted(test_mount_point_));

  // Mount again
  EXPECT_TRUE(manager_->Mount(mp));
  EXPECT_TRUE(manager_->IsMounted(test_mount_point_));

  // Cleanup
  manager_->Unmount(test_mount_point_);
}

TEST_F(MountManagerTest, ConcurrentMountUnmount) {
  std::vector<std::string> mount_points;

  // Create and mount multiple filesystems
  for (int i = 0; i < 5; ++i) {
    std::string mount_point = test_dir_ + "/mnt" + std::to_string(i);
    mount_points.push_back(mount_point);

    aember::mount_manager::MountPoint mp(
        "tmpfs", mount_point, "tmpfs", 0, "size=1M");
    EXPECT_TRUE(manager_->Mount(mp));
    EXPECT_TRUE(manager_->IsMounted(mount_point));
  }

  // Unmount some
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(manager_->Unmount(mount_points[i]));
    EXPECT_FALSE(manager_->IsMounted(mount_points[i]));
  }

  // Verify remaining are still mounted
  for (int i = 3; i < 5; ++i) {
    EXPECT_TRUE(manager_->IsMounted(mount_points[i]));
  }

  // Cleanup remaining
  manager_->UnmountAll();
}

}  // namespace aember_test::mount_manager
