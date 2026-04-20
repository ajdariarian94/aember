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

#include <string>

namespace aember::utils::module {

/**
 * @brief Result of a single module load attempt.
 */
struct ModuleResult {
  std::string name;    ///< Module name e.g. "bridge"
  bool loaded{false};  ///< true if load succeeded
  std::string error;   ///< Error message if failed
};

}  // namespace aember::utils::module
