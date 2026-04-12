/**
 * @file service-type.h
 * @author Arian Ajdari
 * @brief Enum class for ServiceState
 * @version 0.1
 * @date 2026-04-12
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/service/restart-policy.h>
#include <aember-libs/utils/service/service-type.h>
#include <aember-libs/utils/container/container-spec.h>

#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace aember::utils::service {

/**
 * @brief Configuration data for a service.
 */
struct ServiceConfig {
  std::string name;                                ///< Service name
  std::string command;                             ///< Command to execute
  std::vector<std::string> args;                   ///< Command-line arguments
  std::map<std::string, std::string> environment;  ///< Environment variables
  std::string working_directory;  ///< Working directory for the service
  RestartPolicy restart_policy =
      RestartPolicy::NEVER;  ///< Restart behavior
  std::vector<std::string>
      dependencies;              ///< Services that must start before this one
  int max_restart_attempts = 5;  ///< Maximum number of automatic restarts
  std::chrono::seconds restart_delay{5};   ///< Delay between restarts
  ServiceType type{ServiceType::PROCESS};  ///< Process or container
  std::optional<aember::utils::container::ContainerSpec> container;  ///< Optional container spec

  ServiceConfig() = default;
  ServiceConfig(const std::string& n, const std::string& cmd);
};

}  // namespace aember::utils::service
