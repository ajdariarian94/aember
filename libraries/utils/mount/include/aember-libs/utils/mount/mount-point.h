/**
 * @file mount-point.h
 * @author Arian Ajdari
 * @brief Enum class for BridgeConfig
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::utils::mount {

/**
 * @brief Represents a single mount point with its properties.
 */
struct MountPoint {
  std::string source;   ///< What to mount (e.g., "proc", "none")
  std::string target;   ///< Mount point path (e.g., "/proc")
  std::string fstype;   ///< Filesystem type (e.g., "proc", "sysfs")
  unsigned long flags;  ///< Mount flags (MS_NOEXEC, MS_NOSUID, etc.)
  std::string data;     ///< Mount options (e.g., "mode=0755")

  MountPoint(const std::string& src, const std::string& tgt,
             const std::string& type, unsigned long f = 0,
             const std::string& d = "")
      : source(src), target(tgt), fstype(type), flags(f), data(d) {}
};

}  // namespace aember::utils::mount
