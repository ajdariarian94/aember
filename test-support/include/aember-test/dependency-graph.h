/**
 * @file dependency-graph.h
 * @author Arian Ajdari
 * @brief Random dependency graphs for property tests, and the reference
 *        answers to check a start order or a parallel start against.
 *
 * A graph is name -> its direct dependencies, the shape of services.json /
 * containers.json "dependencies" (DependencyResolver::DependencyGraph):
 *
 *   aember::test::GraphOptions opt;   // sizes, cycles, unknown names
 *   const auto graph = aember::test::RandomGraph(g, opt);
 *   const auto good = aember::test::Startable(graph);
 *   EXPECT_TRUE(aember::test::DependenciesFirst(order, graph));
 *
 * Startable() is the meaning spec/tla/StartGraph.tla gives the graph: an
 * entry can start once all its dependencies run, so entries in a cycle, or
 * depending on an unknown name or on such an entry, never start; every
 * other entry does. A scheduler (today ResolveStartOrder, later the
 * parallel start) must start exactly those, each after its dependencies.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <aember-test/property.h>

namespace aember::test {

using DependencyGraph = std::map<std::string, std::vector<std::string>>;

struct GraphOptions {
  int max_nodes = 10;
  /// Chance of an edge from an entry to each entry before it (a DAG).
  double edge_chance = 0.3;
  /// Chance of the graph getting 1..3 back edges (cycles, self-loops too).
  double cycle_chance = 0.3;
  /// Chance of an entry depending on a name that isn't in the graph.
  double unknown_chance = 0.1;
  /// Chance of an entry listing one of its dependencies twice.
  double duplicate_chance = 0.1;
};

/// Names "s0".."sN" in random order of creation; edges go to entries made
/// earlier (a DAG), plus optional back edges, unknown names and duplicates.
inline DependencyGraph RandomGraph(Gen& g, const GraphOptions& opt = {}) {
  const int n = g.Int(0, opt.max_nodes);
  std::vector<std::string> names;
  for (int i = 0; i < n; ++i) names.push_back("s" + std::to_string(i));
  std::ranges::shuffle(names, g.Engine());  // map order != creation order
  DependencyGraph graph;
  for (int i = 0; i < n; ++i) {
    auto& deps = graph[names[static_cast<std::size_t>(i)]];
    for (int j = 0; j < i; ++j) {
      if (g.Chance(opt.edge_chance)) {
        deps.push_back(names[static_cast<std::size_t>(j)]);
      }
    }
    if (g.Chance(opt.unknown_chance)) deps.push_back("missing");
    if (!deps.empty() && g.Chance(opt.duplicate_chance)) {
      deps.push_back(g.Pick(deps));
    }
    std::ranges::shuffle(deps, g.Engine());
  }
  if (n > 0 && g.Chance(opt.cycle_chance)) {
    for (int k = g.Int(1, 3); k > 0; --k) {  // an edge back: a cycle
      const int from = g.Int(0, n - 1);
      const int to = g.Int(from, n - 1);  // to == from: a self-loop
      graph[names[static_cast<std::size_t>(from)]].push_back(
          names[static_cast<std::size_t>(to)]);
    }
  }
  return graph;
}

/// A graph without cycles or unknown names: everything can start.
inline DependencyGraph RandomDag(Gen& g, int max_nodes = 10) {
  return RandomGraph(g,
                     {.max_nodes = max_nodes,
                      .cycle_chance = 0,
                      .unknown_chance = 0,
                      .duplicate_chance = 0.1});
}

/// The entries that can ever start (reference: a fixpoint over the graph).
inline std::set<std::string> Startable(const DependencyGraph& graph) {
  std::set<std::string> good;
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto& [name, deps] : graph) {
      if (good.contains(name)) continue;
      if (std::ranges::all_of(
              deps, [&](const std::string& d) { return good.contains(d); })) {
        good.insert(name);
        changed = true;
      }
    }
  }
  return good;
}

/// Every entry of @p order once, each after all of its dependencies.
inline bool DependenciesFirst(const std::vector<std::string>& order,
                              const DependencyGraph& graph) {
  std::set<std::string> started;
  for (const auto& name : order) {
    if (started.contains(name)) return false;  // twice
    const auto it = graph.find(name);
    if (it == graph.end()) return false;  // not an entry
    for (const auto& d : it->second) {
      if (!started.contains(d)) return false;
    }
    started.insert(name);
  }
  return true;
}

/// The longest chain of dependencies (1 for an entry without any): the
/// fewest "waves" a parallel start needs. Only for startable entries.
inline int Depth(const DependencyGraph& graph) {
  const auto good = Startable(graph);
  std::map<std::string, int> depth;
  int deepest = 0;
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto& name : good) {
      int d = 1;
      for (const auto& dep : graph.at(name)) {
        d = std::max(d, depth[dep] + 1);
      }
      if (depth[name] != d) {
        depth[name] = d;
        changed = true;
      }
      deepest = std::max(deepest, d);
    }
  }
  return deepest;
}

}  // namespace aember::test
