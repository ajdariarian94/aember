/**
 * @file ip-mode.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/network/bridge-config.h>
#include <aember-libs/utils/network/interface-config.h>

#include <chrono>
#include <string>
#include <vector>

namespace aember::utils::network {

/**
 * @brief Full network configuration loaded from the config file.
 */
struct NetworkConfig {
  std::vector<InterfaceConfig> interfaces;
  std::vector<BridgeConfig> bridges;   ///< Bridge interfaces to create
  std::string ping_target{"8.8.8.8"};  ///< TCP probe target
  std::chrono::milliseconds ping_interval{std::chrono::seconds(10)};
  int ping_retries{3};
  std::chrono::milliseconds dhcp_timeout{std::chrono::seconds(30)};
  int dhcp_retries{3};
};

}  // namespace aember::utils::network
