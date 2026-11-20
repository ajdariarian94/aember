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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/// Coverage builds (AEMBER_COVERAGE, set by instrument.cmake): write this
/// process's gcov counters now. A forked child ends with _exit(), which
/// skips gcov's exit handler, so what it ran would never be counted. (A
/// weak reference doesn't work: it never pulls __gcov_dump out of
/// libgcov.a.) A no-op in other builds.
#ifdef AEMBER_COVERAGE
extern "C" void __gcov_dump(void);
inline void FlushCoverage() {
  __gcov_dump();
}
#else
inline void FlushCoverage() {}
#endif

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
  rm.SetRootWait(std::chrono::milliseconds{300});
  aember::utils::root::RootConfig cfg;  // no device
  EXPECT_FALSE(rm.MountRealRoot(cfg));
  cfg.device = "/dev/aember-no-such-disk";
  cfg.fstype = "";
  EXPECT_FALSE(rm.MountRealRoot(cfg));
}

// ---------------------------------------------------------------------------
// Root device by LABEL/UUID without udev (Raspberry Pi: root=LABEL=...)
// ---------------------------------------------------------------------------

/// An ext4 image made with mkfs.ext4 (skips if the tool isn't there, e.g.
/// in the VM).
class Ext4ImageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-ext4-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_ / "sys");
    fs::create_directories(dir_ / "dev");
    image_ = dir_ / "dev" / "mmcblk0p2";
    { std::ofstream{image_} << std::string(8 << 20, '\0'); }
    const auto cmd = "mkfs.ext4 -q -F -L aember-root -U " + std::string{kUuid} +
                     " " + image_.string() + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) GTEST_SKIP() << "no mkfs.ext4";
    // /sys/class/block lists device names; contents don't matter here.
    fs::create_directories(dir_ / "sys" / "mmcblk0p2");
    fs::create_directories(dir_ / "sys" / "mmcblk0p1");
    { std::ofstream{dir_ / "dev" / "mmcblk0p1"} << std::string(4096, 'x'); }
  }
  void TearDown() override { fs::remove_all(dir_); }

  static constexpr const char* kUuid = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
  fs::path dir_;
  fs::path image_;
};

TEST_F(Ext4ImageTest, ReadsLabelAndUuid) {
  const auto id = RootManager::ReadExt4Id(image_.string());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->label, "aember-root");
  EXPECT_EQ(id->uuid, kUuid);
}

TEST_F(Ext4ImageTest, FindsDeviceByLabelAndUuid) {
  const auto sys = (dir_ / "sys").string();
  const auto dev = (dir_ / "dev").string();
  EXPECT_EQ(RootManager::FindBlockDevice("LABEL", "aember-root", sys, dev),
            image_.string());
  EXPECT_EQ(RootManager::FindBlockDevice("UUID", kUuid, sys, dev),
            image_.string());
  EXPECT_EQ(RootManager::FindBlockDevice("LABEL", "other", sys, dev), "");
}

TEST(Ext4IdTest, NotExt4) {
  const auto path = fs::temp_directory_path() /
                    ("aember-notext4-" + std::to_string(::getpid()));
  { std::ofstream{path} << std::string(4096, 'x'); }
  EXPECT_FALSE(RootManager::ReadExt4Id(path.string()).has_value());
  EXPECT_FALSE(RootManager::ReadExt4Id("/nonexistent").has_value());
  fs::remove(path);
}

TEST(RootManagerTest, WaitsForTheRootDevice) {
  RootManager rm{std::make_shared<aember::mount_manager::MountManager>()};
  rm.SetRootWait(std::chrono::milliseconds{400});
  aember::utils::root::RootConfig cfg;
  cfg.device = "LABEL=aember-no-such-label";
  cfg.fstype = "ext4";
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(rm.MountRealRoot(cfg));
  EXPECT_GE(std::chrono::steady_clock::now() - t0,
            std::chrono::milliseconds{350});
}

// ---------------------------------------------------------------------------
// Pivot stages
// ---------------------------------------------------------------------------

TEST(RootManagerTest, PivotRefusesANewRootThatIsNotMounted) {
  RootManager rm{std::make_shared<aember::mount_manager::MountManager>()};
  // No MountRealRoot: the (empty) new root path is no mount point.
  EXPECT_FALSE(rm.PivotToNewRoot());
}

TEST(RootManagerTest, IsInInitramfsWhenThereAreNoServices) {
  // The dev container and the test VM's initramfs have no
  // /etc/aember/services.json; a booted rootfs has.
  EXPECT_EQ(RootManager::IsInInitramfs(),
            !fs::exists("/etc/aember/services.json"));
}

TEST(RootManagerTest, DumpMountInfoDoesNotThrow) {
  RootManager rm{std::make_shared<aember::mount_manager::MountManager>()};
  EXPECT_NO_THROW(rm.DumpMountInfo("test"));
}

/// The whole switch from an initramfs to the real root, as root in a forked
/// child: a tmpfs "initramfs" with /dev, /proc, /sys becomes /, an ext4
/// image (LABEL=aember-pivot; ext4 labels have at most 16 characters) on
/// a writable loop device is found by label,
/// mounted, the essential filesystems are moved and /usr/bin/aember is
/// exec'd from it with --root. That program is a BusyBox script that exits
/// 42 only if it got --root and /proc was moved along.
TEST(RootManagerPivotTest, SwitchesIntoTheRealRootAndExecsAember) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  const char* busybox = std::getenv("AEMBER_TEST_BUSYBOX");
  if (busybox == nullptr || !fs::exists(busybox)) {
    GTEST_SKIP() << "AEMBER_TEST_BUSYBOX (a static BusyBox) not set";
  }

  const fs::path work = fs::path{"/var/tmp"} /
                        ("aember-pivot-test-" + std::to_string(::getpid()));
  const fs::path tree = work / "tree";
  const fs::path image = work / "root.img";
  fs::create_directories(tree / "usr" / "bin");
  fs::create_directories(tree / "bin");
  fs::copy_file(busybox, tree / "bin" / "busybox");
  {
    std::ofstream script{tree / "usr" / "bin" / "aember"};
    script << "#!/bin/busybox sh\n"
              "[ \"$1\" = --root ] && [ -e /proc/self/status ] && exit 42\n"
              "exit 43\n";
  }
  fs::permissions(tree / "usr" / "bin" / "aember", fs::perms::owner_all);
  { std::ofstream{image} << std::string(16 << 20, '\0'); }
  const auto mkfs = "mkfs.ext4 -q -F -L aember-pivot -d " + tree.string() +
                    " " + image.string() + " >/dev/null 2>&1";
  if (std::system(mkfs.c_str()) != 0) {
    fs::remove_all(work);
    GTEST_SKIP() << "no mkfs.ext4 with -d";
  }

  // A writable loop device (MountManager::AttachLoop's are read-only, for
  // SquashFS): the real root is mounted read-write. Loop devices are
  // global, so the parent detaches it after the child is gone.
  std::string loop_dev;
  if (FILE* f = ::popen(("losetup -f --show " + image.string()).c_str(), "r")) {
    char buf[64] = {};
    if (std::fgets(buf, sizeof buf, f) != nullptr) loop_dev = buf;
    ::pclose(f);
  }
  while (!loop_dev.empty() && loop_dev.back() == '\n') loop_dev.pop_back();
  if (loop_dev.empty()) {
    fs::remove_all(work);
    GTEST_SKIP() << "no losetup / free loop device";
  }

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  const fs::path child_log = work / "child.log";
  if (child == 0) {
    // Opened before the chroot: the fd keeps working inside it.
    const int log_fd = ::open(child_log.c_str(), O_WRONLY | O_CREAT, 0644);
    ::dup2(log_fd, STDOUT_FILENO);
    ::dup2(log_fd, STDERR_FILENO);
    if (::unshare(CLONE_NEWNS) != 0 ||
        ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
      ::_exit(10);
    }
    auto mounts = std::make_shared<aember::mount_manager::MountManager>();
    // A tmpfs as the "initramfs", with what the real one has mounted.
    const fs::path initramfs = work / "initramfs";
    fs::create_directories(initramfs);
    if (::mount("tmpfs", initramfs.c_str(), "tmpfs", 0, nullptr) != 0) {
      ::_exit(11);
    }
    for (const auto* d : {"dev", "proc", "sys", "mnt/root"}) {
      fs::create_directories(initramfs / d);
    }
    if (::mount("/dev",
                (initramfs / "dev").c_str(),
                nullptr,
                MS_BIND | MS_REC,
                nullptr) != 0 ||
        ::mount("proc", (initramfs / "proc").c_str(), "proc", 0, nullptr) !=
            0 ||
        ::mount("/sys",
                (initramfs / "sys").c_str(),
                nullptr,
                MS_BIND | MS_REC,
                nullptr) != 0) {
      ::_exit(12);
    }
    { std::ofstream{initramfs / "initramfs-file"} << "freed by SwitchRoot"; }
    if (::chdir(initramfs.c_str()) != 0 ||
        ::mount(".", "/", nullptr, MS_MOVE, nullptr) != 0 ||
        ::chroot(".") != 0 || ::chdir("/") != 0) {
      ::_exit(13);
    }

    RootManager rm{mounts};
    rm.SetRootWait(std::chrono::seconds{5});
    aember::utils::root::RootConfig cfg;
    cfg.device = "LABEL=aember-pivot";
    cfg.fstype = "ext4";
    cfg.new_root_path = "/mnt/root";
    // exec replaces this process: what ran up to SwitchRoot() is counted
    // only if written now (the exec itself can't be).
    FlushCoverage();
    rm.PerformPivot(cfg);  // execs /usr/bin/aember on success
    ::_exit(15);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  std::string log;
  {
    std::ifstream in{child_log};
    log.assign(std::istreambuf_iterator<char>{in}, {});
  }
  std::ignore = std::system(("losetup -d " + loop_dev).c_str());
  fs::remove_all(work);
  ASSERT_TRUE(WIFEXITED(status)) << "status " << status << "\n" << log;
  EXPECT_EQ(WEXITSTATUS(status), 42)
      << "10-15: setup/pivot step failed, 43: aember ran without --root/proc\n"
      << log;
}

}  // namespace
