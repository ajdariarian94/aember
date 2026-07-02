/**
 * @file dependency-resolver.h
 * @author Arian Ajdari
 * @brief DependencyResolver — evaluates service start-order and dependency
 *        satisfaction. Knows nothing about processes, containers, or configs.
 * @version 0.4
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aember::service_manager {

/**
 * DependencyResolver operates purely on names and dependency lists.
 * No knowledge of ProcessConfig, ContainerConfig, ProcessState, or managers.
 */
class DependencyResolver {
 public:
  using Logger = aember::utils::logging::Logger;

  /// Graph type: name → its direct dependencies.
  using DependencyGraph = std::map<std::string, std::vector<std::string>>;

  /**
   * Returns true if the given name is currently running.
   * Typically wraps ServiceManager::GetState.
   * move_only_function — never copied.
   */
  using StateQuery =
      std::move_only_function<bool(const std::string& /*name*/) const>;

  explicit DependencyResolver(StateQuery is_running);

  DependencyResolver(const DependencyResolver&) = delete;
  DependencyResolver& operator=(const DependencyResolver&) = delete;

  [[nodiscard]] bool CheckDependencies(std::string_view name,
                                       const DependencyGraph& graph) const;

  /**
   * Topological sort via Kahn's algorithm.
   * Throws std::runtime_error on a dependency cycle.
   */
  [[nodiscard]] std::vector<std::string> ResolveStartOrder(
      const DependencyGraph& graph) const;

 private:
  StateQuery is_running_;

  mutable Logger log_{"dependency-resolver"};
};

}  // namespace aember::service_manager
