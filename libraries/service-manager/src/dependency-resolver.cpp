/**
 * @file dependency-resolver.cpp
 * @author Arian Ajdari
 * @brief DependencyResolver implementation.
 * @version 0.4
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/dependency-resolver.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace aember::service_manager {

DependencyResolver::DependencyResolver(StateQuery is_running)
    : is_running_(std::move(is_running)) {}

// ---------------------------------------------------------------------------
// CheckDependencies
// ---------------------------------------------------------------------------

bool DependencyResolver::CheckDependencies(std::string_view name,
                                           const DependencyGraph& graph) const {
  auto it = graph.find(std::string{name});
  if (it == graph.end()) {
    log_.error("CheckDependencies: '{}' not in graph", name);
    return false;
  }

  for (const auto& dep : it->second) {
    if (!graph.contains(dep)) {
      log_.error("'{}' has unknown dependency '{}'", name, dep);
      return false;
    }

    if (!is_running_(dep)) {
      log_.warn("Dependency '{}' of '{}' is not running", dep, name);
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// ResolveStartOrder — Kahn's algorithm
// ---------------------------------------------------------------------------

std::vector<std::string> DependencyResolver::ResolveStartOrder(
    const DependencyGraph& graph) const {
  std::unordered_map<std::string, int> in_degree;
  std::unordered_map<std::string, std::vector<std::string>> dependents;

  for (const auto& [name, deps] : graph) {
    if (!in_degree.contains(name)) { in_degree[name] = 0; }
    for (const auto& dep : deps) {
      dependents[dep].push_back(name);
      ++in_degree[name];
    }
  }

  // Seed with zero-dependency names.
  std::vector<std::string> queue;
  for (const auto& [name, degree] : in_degree) {
    if (degree == 0) { queue.push_back(name); }
  }

  std::vector<std::string> order;
  order.reserve(graph.size());

  while (!queue.empty()) {
    const std::string current = std::move(queue.back());
    queue.pop_back();

    for (const auto& dependent : dependents[current]) {
      if (--in_degree[dependent] == 0) { queue.push_back(dependent); }
    }

    order.push_back(current);
  }

  // Entries in a cycle or depending on an unknown name never reach in-degree
  // 0. Skip only those (logged): one bad entry in services.json must not
  // keep every other service from starting.
  if (order.size() != graph.size()) {
    std::string skipped;
    for (const auto& [name, deps] : graph) {
      if (std::ranges::find(order, name) == order.end()) {
        skipped += (skipped.empty() ? "" : ", ") + name;
      }
    }
    log_.error("Not starting (dependency cycle or unknown dependency): {}",
               skipped);
  }

  log_.info("Resolved start order for {} entries", order.size());
  return order;
}

}  // namespace aember::service_manager
