/**
 * @file container-spec.h
 * @author Arian Ajdari
 * @brief Enum class for ContainerPec
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/container/container-config.h>
#include <aember-libs/utils/container/container-state.h>

struct lxc_container;

namespace aember::utils::container {

struct ContainerEntry {
  ContainerConfig config;
  ContainerState state{ContainerState::kStopped};
  lxc_container* lxc{nullptr};
};


}  // namespace aember::utils::service
