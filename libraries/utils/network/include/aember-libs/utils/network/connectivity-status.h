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

#include <string>

namespace aember::utils::network {

/**
 * @brief Connectivity status reported via the status callback.
 */
struct ConnectivityStatus {
  bool online{false};
  int rtt_ms{-1};
  std::string interface;
  int64_t timestamp_ms{0};
};

}  // namespace aember::utils::network
