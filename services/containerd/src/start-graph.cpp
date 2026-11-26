/**
 * @file start-graph.cpp
 * @author Arian Ajdari
 * @brief StartGraph (see start-graph.h).
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-services/containerd/start-graph.h>

#include <algorithm>
#include <numeric>
#include <ranges>

namespace aember::containerd {

StartGraph::StartGraph(const std::vector<Node>& nodes) {
  // Duplicates: the first entry counts, the others are skipped.
  std::map<std::string, std::vector<std::string>> all;
  std::map<std::string, std::vector<std::string>> services;
  std::vector<std::string> order;
  for (const auto& node : nodes) {
    if (all.contains(node.name)) {
      skipped_[node.name + " (again)"] = "listed twice";
      continue;
    }
    all[node.name] = node.dependencies;
    services[node.name] = node.services;
    order.push_back(node.name);
  }

  // The entries that can ever start: a fixpoint (everything a cycle, an
  // unknown name or a skipped entry is behind never gets in).
  std::set<std::string> good;
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto& name : order) {
      if (good.contains(name)) continue;
      if (std::ranges::all_of(all[name], [&](const std::string& d) {
            return good.contains(d);
          })) {
        good.insert(name);
        changed = true;
      }
    }
  }
  for (const auto& name : order) {
    if (good.contains(name)) {
      names_.push_back(name);
      deps_[name] = all[name];
      services_[name] = services[name];
      continue;
    }
    std::string why;
    for (const auto& d : all[name]) {
      if (good.contains(d)) continue;
      if (d == name) {
        why = "depends on itself";
      } else if (!all.contains(d)) {
        why = "needs '" + d + "', which isn't listed";
      } else {
        why = "needs '" + d + "', which can't start (a dependency cycle)";
      }
      break;
    }
    skipped_[name] = why;
  }
}

bool StartGraph::Has(const std::string& name) const {
  return deps_.contains(name);
}

void StartGraph::Want(const std::string& name) {
  if (Has(name) && !running_.contains(name) && !starting_.contains(name)) {
    wanted_.insert(name);
  }
}

void StartGraph::Unwant(const std::string& name) {
  wanted_.erase(name);
}

std::vector<std::string> StartGraph::TakeReady() {
  std::vector<std::string> ready;
  for (const auto& name : names_) {  // file order: deterministic
    if (!wanted_.contains(name)) continue;
    if (DependenciesRun(name)) ready.push_back(name);
  }
  for (const auto& name : ready) {
    wanted_.erase(name);
    starting_.insert(name);
  }
  return ready;
}

void StartGraph::SetRunning(const std::string& name, bool running) {
  starting_.erase(name);
  if (running && Has(name)) {
    running_.insert(name);
  } else {
    running_.erase(name);
  }
}

void StartGraph::SetServiceRunning(const std::string& service, bool running) {
  if (running) {
    services_running_.insert(service);
  } else {
    services_running_.erase(service);
  }
}

std::set<std::string> StartGraph::NeededServices() const {
  std::set<std::string> needed;
  for (const auto& [name, list] : services_)
    needed.insert(list.begin(), list.end());
  return needed;
}

bool StartGraph::DependenciesRun(const std::string& name) const {
  const auto it = deps_.find(name);
  if (it == deps_.end()) return false;
  const auto& services = services_.at(name);
  return std::ranges::all_of(
             it->second,
             [&](const std::string& d) { return running_.contains(d); }) &&
         std::ranges::all_of(services, [&](const std::string& s) {
           return services_running_.contains(s);
         });
}

bool StartGraph::IsRunning(const std::string& name) const {
  return running_.contains(name);
}

bool StartGraph::IsWanted(const std::string& name) const {
  return wanted_.contains(name);
}

bool StartGraph::IsStarting(const std::string& name) const {
  return starting_.contains(name);
}

std::vector<std::string> StartGraph::StopOrder() const {
  // A topological order (dependencies first), reversed.
  std::vector<std::string> order;
  std::set<std::string> placed;
  while (order.size() < names_.size()) {
    for (const auto& name : names_) {
      if (placed.contains(name)) continue;
      const auto& deps = deps_.at(name);
      if (std::ranges::all_of(
              deps, [&](const std::string& d) { return placed.contains(d); })) {
        order.push_back(name);
        placed.insert(name);
      }
    }
  }
  std::ranges::reverse(order);
  return order;
}

int StartGraph::Depth() const {
  // Dependencies first (the stop order reversed): each one's depth is
  // known when its dependents need it.
  std::map<std::string, int> depth;
  for (const auto& name : std::views::reverse(StopOrder())) {
    const auto& deps = deps_.at(name);
    depth[name] = 1 + std::accumulate(deps.begin(),
                                      deps.end(),
                                      0,
                                      [&](int deepest, const std::string& d) {
                                        return std::max(deepest, depth[d]);
                                      });
  }
  return std::accumulate(
      depth.begin(), depth.end(), 0, [](int deepest, const auto& entry) {
        return std::max(deepest, entry.second);
      });
}

}  // namespace aember::containerd
