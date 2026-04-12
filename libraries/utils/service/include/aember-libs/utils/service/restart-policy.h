/**
 * @file restart-policy.h
 * @author Arian Ajdari
 * @brief Enum class for RestartPolicy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::service {

/**
 * @brief Defines when a service should be restarted after it stops.
 */
enum class RestartPolicy {
  NEVER,       ///< Do not restart automatically
  ON_FAILURE,  ///< Restart only if the service failed
  ALWAYS       ///< Always restart regardless of exit code
};

/**
 * @brief Convert a RestartPolicy enum to a human-readable string.
 */
std::string RestartPolicyToString(aember::utils::service::RestartPolicy policy);

}  // namespace aember::utils::service
