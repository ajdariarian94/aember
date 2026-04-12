/**
 * @file container-state.cpp
 * @author Arian Ajdari
 * @brief Library implementation for restart policy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/container/container-state.h>

namespace aember::utils::container {

std::string ContainerStateToString(ContainerState s) {
  switch (s) {
    case ContainerState::kStopped:
      return "stopped";
    case ContainerState::kStarting:
      return "starting";
    case ContainerState::kRunning:
      return "running";
    case ContainerState::kStopping:
      return "stopping";
    case ContainerState::kFailed:
      return "failed";
    default:
      return "unknown";
  }
}

}  // namespace aember::utils::service
