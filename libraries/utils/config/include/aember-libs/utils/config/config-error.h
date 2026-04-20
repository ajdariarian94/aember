/**
 * @file config-manager.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Configuration loader and validator for service definitions.
 *
 * The ConfigManager is responsible for loading, validating, and parsing
 * service configuration files. It converts JSON-based configuration data
 * into ServiceConfig objects consumable by the ServiceManager.
 *
 * This component performs no service lifecycle actions; it only provides
 * validated configuration data.
 */

#pragma once

#include <string>

namespace aember::utils::config {

/**
 * @brief Describes a configuration parsing or validation error.
 *
 * Used to report human-readable error information when loading or
 * validating configuration data.
 */
struct ConfigError {
  std::string message;  // Error description
  std::string file;     // Source file (if applicable)
  int line = -1;        // Line number (if available)

  ConfigError() = default;

  explicit ConfigError(const std::string& msg) : message(msg) {}
  ConfigError(const std::string& msg, const std::string& f, int l = -1)
      : message(msg), file(f), line(l) {}
};

}
