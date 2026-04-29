/**
 * @file module-result.h
 * @author Arian Ajdari
 * @brief Enum class for MountPoint
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/module/module-error-code.h>
#include <aember-libs/utils/module/module-status.h>

#include <string>

namespace aember::utils::module {

/**
 * @brief Result of a single module load attempt.
 */
struct ModuleResult {
  std::string name;
  ModuleStatus status{ModuleStatus::Failed};
  ModuleErrorCode code{ModuleErrorCode::Unknown};
  std::string error;
};

}  // namespace aember::utils::module
