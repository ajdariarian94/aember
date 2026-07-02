/**
 * @file container-config.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>
#include <vector>

namespace aember::utils::container {

struct ContainerConfig {
  std::string name;
  std::string squashfs;
  std::string rootfs;
  std::string config_path;
  std::vector<std::string> args;
};

}  // namespace aember::utils::container
