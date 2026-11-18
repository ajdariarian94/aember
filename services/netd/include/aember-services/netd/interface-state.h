/**
 * @file interface-state.h
 * @author Arian Ajdari
 * @brief State of a network interface
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

namespace aember::netd {

/**
 * @brief Runtime state of a network interface.
 */
enum class InterfaceState {
  kDown,        ///< Interface not yet brought up
  kBringingUp,  ///< Getting a DHCP lease / applying static config
  kUp,          ///< Interface is up and has an address
  kFailed,      ///< Failed to come up after all retries
};

}  // namespace aember::netd
