/**
 * @file restart-policy.h
 * @author Arian Ajdari
 * @brief Enum class for ServiceState
 * @version 0.1
 * @date 2026-04-12
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::service {

/**
 * @brief Possible states a service can be in.
 */
enum class ServiceState {
  STOPPED,   ///< Service is not running
  STARTING,  ///< Service is starting up
  RUNNING,   ///< Service is currently running
  STOPPING,  ///< Service is stopping
  FAILED     ///< Service failed (crashed or exited with error)
};

/**
 * @brief Convert a ServiceState enum to a human-readable string.
 */
std::string ServiceStateToString(ServiceState state);

}  // namespace aember::utils::service
