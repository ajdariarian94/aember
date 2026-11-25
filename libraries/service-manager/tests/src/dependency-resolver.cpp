/**
 * @file dependency-resolver.cpp
 * @author Arian Ajdari
 * @brief Property tests for DependencyResolver on random dependency graphs
 *        (cycles, self-loops, unknown names, duplicates), checked against
 *        the reference answers in aember-test/dependency-graph.h.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/dependency-resolver.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

#include <aember-test/dependency-graph.h>

namespace {

using aember::service_manager::DependencyResolver;
using aember::test::Gen;

TEST(DependencyResolverProperty, StartsExactlyWhatCanStartDependenciesFirst) {
  aember::test::ForAll(500, [](Gen& g) {
    const auto graph = aember::test::RandomGraph(g);
    const DependencyResolver resolver{[](const std::string&) { return true; }};
    const auto order = resolver.ResolveStartOrder(graph);
    EXPECT_TRUE(aember::test::DependenciesFirst(order, graph));
    EXPECT_EQ(std::set<std::string>(order.begin(), order.end()),
              aember::test::Startable(graph));
  });
}

TEST(DependencyResolverProperty, ADagStartsEverything) {
  aember::test::ForAll(300, [](Gen& g) {
    const auto graph = aember::test::RandomDag(g);
    const DependencyResolver resolver{[](const std::string&) { return true; }};
    EXPECT_EQ(resolver.ResolveStartOrder(graph).size(), graph.size());
  });
}

TEST(DependencyResolverProperty, CheckDependenciesMeansAllKnownAndRunning) {
  aember::test::ForAll(300, [](Gen& g) {
    const auto graph = aember::test::RandomGraph(g);
    std::set<std::string> running;
    for (const auto& [name, deps] : graph) {
      if (g.Chance(0.6)) running.insert(name);
    }
    const DependencyResolver resolver{
        [&](const std::string& name) { return running.contains(name); }};
    for (const auto& [name, deps] : graph) {
      bool expected = true;
      for (const auto& d : deps) {
        expected = expected && graph.contains(d) && running.contains(d);
      }
      EXPECT_EQ(resolver.CheckDependencies(name, graph), expected) << name;
    }
    EXPECT_FALSE(resolver.CheckDependencies("not-in-the-graph", graph));
  });
}

// The generator itself: what the property tests above (and the parallel
// start's, later) rely on.
TEST(DependencyGraphGenerator, MakesCyclesUnknownNamesAndDags) {
  int with_cycle_or_unknown = 0;
  int deep = 0;
  for (int i = 0; i < 300; ++i) {  // a fixed count (not ForAll's)
    Gen g{aember::test::BaseSeed() + static_cast<std::uint64_t>(i)};
    const auto graph = aember::test::RandomGraph(g);
    if (aember::test::Startable(graph).size() != graph.size()) {
      ++with_cycle_or_unknown;
    }
    if (aember::test::Depth(graph) >= 3) ++deep;
    const auto dag = aember::test::RandomDag(g);
    EXPECT_EQ(aember::test::Startable(dag).size(), dag.size());
  }
  EXPECT_GT(with_cycle_or_unknown, 10);
  EXPECT_GT(deep, 10);
}

}  // namespace
