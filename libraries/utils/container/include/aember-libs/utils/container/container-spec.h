/**
 * @file container-spec.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerPec
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>
#include <vector>

namespace aember::utils::container {

/**
 * @brief Specifications for container-based services.
 */
struct ContainerSpec {
  std::string rootfs;  ///< Root filesystem path for the container
  std::string squashfs;
  std::vector<std::string> args;  ///< Arguments passed to container runtime
};

}  // namespace aember::utils::container
