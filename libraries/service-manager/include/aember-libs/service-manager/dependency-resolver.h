/**
 * @file dependency-resolver.h
 * @author Arian Ajdari
 * @brief DependencyResolver — evaluates service start-order and dependency
 *        satisfaction. Knows nothing about processes, containers, or configs.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aember::service_manager {

/**
 * DependencyResolver operates purely on names and dependency lists.
 *
 * It has no knowledge of ProcessConfig, ContainerConfig, ProcessState,
 * or any lifecycle manager. The caller provides:
 *
 *  - a dependency graph: name → list of names it depends on
 *  - a StateQuery lambda to check whether a name is currently running
 */
class DependencyResolver {
 public:
  using Logger = aember::utils::logging::Logger;

  /// Graph type: name → its direct dependencies.
  using DependencyGraph = std::map<std::string, std::vector<std::string>>;

  /**
   * Returns true if the given name is currently in the Running state.
   * Typically a lambda wrapping ServiceManager::GetState.
   */
  using StateQuery = std::function<bool(const std::string& /*name*/)>;

  explicit DependencyResolver(StateQuery is_running);

  DependencyResolver(const DependencyResolver&) = delete;
  DependencyResolver& operator=(const DependencyResolver&) = delete;

  // ---------------------------------------------------------------------------
  // Core API
  // ---------------------------------------------------------------------------

  /**
   * Returns true when every dependency of @p name is currently running
   * according to the injected StateQuery.
   *
   * @param name  Service to check.
   * @param graph Full dependency graph (name → dependencies).
   */
  [[nodiscard]] bool CheckDependencies(const std::string& name,
                                       const DependencyGraph& graph) const;

  /**
   * Returns the names in @p graph topologically sorted so that each
   * name appears after all its dependencies.
   *
   * Throws std::runtime_error on a dependency cycle.
   */
  [[nodiscard]] std::vector<std::string> ResolveStartOrder(
      const DependencyGraph& graph) const;

 private:
  StateQuery is_running_;

  mutable Logger log_{"dependency-resolver"};
};

}  // namespace aember::service_manager
