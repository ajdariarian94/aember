/**
 * @file mount-point.h
 * @author Arian Ajdari
 * @brief Enum class for BridgeConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::root {

struct RootConfig {
  std::string device;         // "/dev/sda1", "UUID=xxx", "LABEL=xxx"
  std::string fstype;         // "ext4", "btrfs", etc.
  std::string mount_options;  // "ro,noatime"
  std::string new_root_path;  // mount target (default "/mnt/root")

  explicit RootConfig();

  void ParseFromProcCmdline(const std::string& path = "/proc/cmdline");
};

}  // namespace aember::utils::network
