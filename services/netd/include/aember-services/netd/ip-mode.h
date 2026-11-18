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

namespace aember::netd {

/**
 * @brief IP configuration mode for a network interface.
 */
enum class IpMode {
  kDhcp,    ///< Obtain address via DHCP (built-in client)
  kStatic,  ///< Use a statically configured address
};

}  // namespace aember::netd
