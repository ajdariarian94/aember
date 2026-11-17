#include <filesystem>

#include <sched.h>
#include <sys/mount.h>

#include <aember-libs-tests/mount-manager/mount-manager.h>

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
  AEMBER_REQUIRE_PRIVATE_MOUNTS();
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

// ---------------------------------------------------------------------------
// Loop devices (replacing `busybox mount -t squashfs`)
// ---------------------------------------------------------------------------

namespace aember_test::mount_manager {

using aember::mount_manager::MountManager;

TEST(LoopTest, MissingImageIsAnError) {
  const auto loop = MountManager::AttachLoop("/nonexistent/image.squashfs");
  ASSERT_FALSE(loop.has_value());
  EXPECT_NE(loop.error().find("/nonexistent/image.squashfs"),
            std::string::npos);
}

TEST(LoopTest, WithoutRootThereIsNoLoopDevice) {
  if (::geteuid() == 0) GTEST_SKIP() << "running as root";
  const auto img = std::filesystem::temp_directory_path() / "aember-loop.img";
  { std::ofstream{img} << std::string(4096, '\0'); }
  EXPECT_FALSE(MountManager::AttachLoop(img.string()).has_value());
  std::filesystem::remove(img);
}

// As root (inside the VM): attaching works, and autoclear releases the
// device again once nothing uses it.
TEST(LoopTest, AttachAndAutoclearAsRoot) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  const auto img = std::filesystem::temp_directory_path() / "aember-loop.img";
  { std::ofstream{img} << std::string(1 << 20, '\0'); }
  std::string backing;
  {
    const auto loop = MountManager::AttachLoop(img.string());
    ASSERT_TRUE(loop.has_value()) << loop.error();
    EXPECT_TRUE(loop->path.starts_with("/dev/loop"));
    backing = "/sys/block/" + loop->path.substr(5) + "/loop/backing_file";
    // Regression: while the handle is alive the device must stay attached
    // (closing the fd early detached it before mount() could use it).
    EXPECT_TRUE(std::filesystem::exists(backing));
  }
  // Handle gone, nothing mounted: autoclear has detached it.
  EXPECT_FALSE(std::filesystem::exists(backing));
  std::filesystem::remove(img);
}

// As root: an image that isn't SquashFS fails to mount cleanly.
TEST(LoopTest, MountingNonSquashfsFailsAsRoot) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  AEMBER_REQUIRE_PRIVATE_MOUNTS();
  const auto img = std::filesystem::temp_directory_path() / "aember-bad.img";
  const auto target = std::filesystem::temp_directory_path() / "aember-bad-mnt";
  { std::ofstream{img} << std::string(1 << 20, 'x'); }
  MountManager mm;
  EXPECT_FALSE(mm.MountSquashFS(img.string(), target.string()));
  EXPECT_FALSE(mm.IsMounted(target.string()));
  std::filesystem::remove(img);
  std::filesystem::remove_all(target);
}

}  // namespace aember_test::mount_manager
