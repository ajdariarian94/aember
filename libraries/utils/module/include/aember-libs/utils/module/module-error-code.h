/**
 * @file module-error-code.h
 * @author Arian Ajdari
 * @brief Enum class for MountPoint
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

namespace aember::utils::module {

enum class ModuleErrorCode {
  None,
  LoadFailed,  ///< finit_module(2) failed for the module or a dependency
  NotFound,    ///< not in modules.dep (and not built in)
  InvalidInput,
  Unknown
};
}  // namespace aember::utils::module
