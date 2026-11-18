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

#include <aember-services/netd/interface-config.h>

#include <chrono>
#include <string>
#include <vector>

namespace aember::netd {

/**
 * @brief Full network configuration loaded from the config file.
 */
struct NetworkConfig {
  std::vector<InterfaceConfig> interfaces;
  std::string ping_target{"8.8.8.8"};  ///< TCP probe target
  std::chrono::milliseconds ping_interval{std::chrono::seconds(10)};
  int ping_retries{3};
  std::chrono::milliseconds dhcp_timeout{std::chrono::seconds(30)};
  int dhcp_retries{3};
};

}  // namespace aember::netd
