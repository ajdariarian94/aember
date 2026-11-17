/**
 * @file root-config.cpp
 * @author Arian Ajdari
 * @brief RootConfig tests: root=, rootfstype=, rootflags= from the kernel
 *        command line, and the defaults.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/root/root-config.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::utils::root::RootConfig;

std::string Cmdline(const std::string& content) {
  const auto path = fs::temp_directory_path() /
                    ("aember-root-cmdline-" + std::to_string(::getpid()));
  std::ofstream{path} << content;
  return path.string();
}

TEST(RootConfigTest, Defaults) {
  RootConfig cfg;
  EXPECT_EQ(cfg.new_root_path, "/mnt/root");
  EXPECT_TRUE(cfg.device.empty());
}

TEST(RootConfigTest, RootOnlyGetsExt4Rw) {
  RootConfig cfg;
  cfg.ParseFromProcCmdline(Cmdline("console=ttyS0 root=/dev/sda1 rootwait\n"));
  EXPECT_EQ(cfg.device, "/dev/sda1");
  EXPECT_EQ(cfg.fstype, "ext4");
  EXPECT_EQ(cfg.mount_options, "rw");
}

TEST(RootConfigTest, AllRootParameters) {
  RootConfig cfg;
  cfg.ParseFromProcCmdline(Cmdline(
      "root=/dev/vda1 rootfstype=btrfs rootflags=ro,noatime aember.shell=1"));
  EXPECT_EQ(cfg.device, "/dev/vda1");
  EXPECT_EQ(cfg.fstype, "btrfs");
  EXPECT_EQ(cfg.mount_options, "ro,noatime");
}

TEST(RootConfigTest, UuidAndLabelValuesKeepTheirEquals) {
  RootConfig cfg;
  cfg.ParseFromProcCmdline(Cmdline("root=UUID=1234-abcd quiet"));
  EXPECT_EQ(cfg.device, "UUID=1234-abcd");
  RootConfig label;
  label.ParseFromProcCmdline(Cmdline("root=LABEL=rootfs"));
  EXPECT_EQ(label.device, "LABEL=rootfs");
}

TEST(RootConfigTest, LastRootWins) {
  RootConfig cfg;
  cfg.ParseFromProcCmdline(Cmdline("root=/dev/sda1 root=/dev/vda1"));
  EXPECT_EQ(cfg.device, "/dev/vda1");
}

TEST(RootConfigTest, MissingRootThrows) {
  RootConfig cfg;
  EXPECT_THROW(cfg.ParseFromProcCmdline(Cmdline("console=ttyS0 quiet")),
               std::runtime_error);
  EXPECT_THROW(cfg.ParseFromProcCmdline(Cmdline("")), std::runtime_error);
}

TEST(RootConfigTest, MissingFileThrows) {
  RootConfig cfg;
  EXPECT_THROW(cfg.ParseFromProcCmdline("/nonexistent/cmdline"),
               std::runtime_error);
}

}  // namespace
