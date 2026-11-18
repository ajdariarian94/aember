/**
 * @file network-info.h
 * @author Arian Ajdari
 * @brief Address information of the network
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>
#include <vector>

namespace aember::netd {

/**
 * @brief Snapshot of the current network state.
 */
struct NetworkInfo {
  std::string interface;
  std::string ip_address;
  std::string netmask;
  int prefix_len{0};
  std::string broadcast;
  std::string gateway;
  std::string mac_address;
  std::vector<std::string> dns_servers;
  bool online{false};
  int rtt_ms{-1};
};

}  // namespace aember::netd
