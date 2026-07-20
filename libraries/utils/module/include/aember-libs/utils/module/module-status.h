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

enum class ModuleStatus { Loaded, AlreadyLoaded, Failed };

}  // namespace aember::utils::module
