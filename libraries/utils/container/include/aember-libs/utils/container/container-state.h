/**
 * @file container-state.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerState
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::container {

enum class ContainerState {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
  kFailed,
};

std::string ContainerStateToString(ContainerState state);

}  // namespace aember::utils::container
