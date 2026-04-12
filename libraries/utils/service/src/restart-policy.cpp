/**
 * @file restart-policy.cpp
 * @author Arian Ajdari
 * @brief Library implementation for restart policy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/service/restart-policy.h>

namespace aember::utils::service {

/**
 * @brief Convert a RestartPolicy enum to a human-readable string.
 *
 * @param policy RestartPolicy value
 * @return std::string Human-readable string
 */
std::string RestartPolicyToString(RestartPolicy policy) {
  switch (policy) {
    case RestartPolicy::NEVER:
      return "NEVER";
    case RestartPolicy::ON_FAILURE:
      return "ON_FAILURE";
    case RestartPolicy::ALWAYS:
      return "ALWAYS";
    default:
      return "UNKNOWN";
  }
}

}  // namespace aember::utils::service
