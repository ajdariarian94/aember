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
 * @brief Runtime state of a network interface.
 */
enum class InterfaceState {
  kDown,        ///< Interface not yet brought up
  kBringingUp,  ///< Currently running udhcpc / applying static config
  kUp,          ///< Interface is up and has an address
  kFailed,      ///< Failed to come up after all retries
};

}  // namespace aember::utils::network
