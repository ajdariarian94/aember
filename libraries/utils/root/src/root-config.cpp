/**
 * @file root-config.cpp
 * @author Arian Ajdari
 * @brief Library implementation for service config
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/root/root-config.h>

#include <fstream>
#include <sstream>

namespace aember::utils::root {

RootConfig::RootConfig() : new_root_path("/mnt/root") {}

void RootConfig::ParseFromProcCmdline(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) { throw std::runtime_error("Failed to open " + path); }

  std::string line;
  std::getline(file, line);

  std::istringstream iss(line);
  std::string token;

  while (iss >> token) {
    auto pos = token.find('=');
    if (pos == std::string::npos) continue;

    std::string key = token.substr(0, pos);
    std::string value = token.substr(pos + 1);

    if (key == "root") {
      device = value;
    } else if (key == "rootfstype") {
      fstype = value;
    } else if (key == "rootflags") {
      mount_options = value;
    }
  }

  if (device.empty()) {
    throw std::runtime_error("Kernel parameter 'root=' is missing");
  }

  if (fstype.empty()) { fstype = "ext4"; }

  if (mount_options.empty()) { mount_options = "rw"; }
}

}  // namespace aember::utils::root
