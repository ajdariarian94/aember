/**
 * @file service-state.cpp
 * @author Arian Ajdari
 * @brief Library implementation for restart policy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/service/service-state.h>

namespace aember::utils::service {

/**
 * @brief Convert a ServiceState enum to a human-readable string.
 *
 * @param state ServiceState value
 * @return std::string Human-readable string
 */
std::string ServiceStateToString(ServiceState state) {
  switch (state) {
    case ServiceState::STOPPED:
      return "STOPPED";
    case ServiceState::STARTING:
      return "STARTING";
    case ServiceState::RUNNING:
      return "RUNNING";
    case ServiceState::STOPPING:
      return "STOPPING";
    case ServiceState::FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

}  // namespace aember::utils::service
