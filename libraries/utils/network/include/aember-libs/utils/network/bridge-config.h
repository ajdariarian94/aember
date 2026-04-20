/**
 * @file bridge-config.h
 * @author Arian Ajdari
 * @brief Enum class for BridgeConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::network {

/**
 * @brief Configuration for a Linux bridge interface, loaded from JSON.
 */
struct BridgeConfig {
  std::string name;     ///< Bridge name e.g. "lxcbr0"
  std::string address;  ///< IP in CIDR e.g. "10.0.3.1/24"
};

}  // namespace aember::utils::network
