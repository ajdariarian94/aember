/**
 * @file interface-config.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/network/ip-mode.h>

#include <string>
#include <vector>

namespace aember::utils::network {

/**
 * @brief Configuration for a single network interface, loaded from JSON.
 */
struct InterfaceConfig {
  std::string name;            ///< e.g. "eth0"
  IpMode mode{IpMode::kDhcp};  ///< DHCP or static
  std::string address;         ///< Static IP in CIDR, e.g. "192.168.1.10/24"
  std::string gateway;         ///< Default gateway, e.g. "192.168.1.1"
  std::vector<std::string>
      dns_servers;       ///< DNS servers to write to resolv.conf
  bool required{false};  ///< If true, init blocks until this IF is up
};

}  // namespace aember::utils::network
