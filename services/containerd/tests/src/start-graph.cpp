/**
 * @file start-graph.cpp
 * @author Arian Ajdari
 * @brief StartGraph: which containers may start, as spec/tla/StartGraph.tla
 *        designs it; checked against the reference answers of
 *        test-support's dependency graphs.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-services/containerd/start-graph.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include <aember-test/dependency-graph.h>
#include <aember-test/property.h>

namespace {

using aember::containerd::StartGraph;
using aember::test::DependencyGraph;
using aember::test::Gen;

std::vector<StartGraph::Node> Nodes(const DependencyGraph& graph) {
  std::vector<StartGraph::Node> nodes;
  for (const auto& [name, deps] : graph) nodes.push_back({name, deps});
  return nodes;
}

/// Start everything wanted, wave by wave (each wave starts together and
/// comes up): the order and the number of waves.
std::pair<std::vector<std::string>, int> StartAll(StartGraph& g) {
  for (const auto& n : g.Names()) g.Want(n);
  std::vector<std::string> order;
  int waves = 0;
  for (auto ready = g.TakeReady(); !ready.empty(); ready = g.TakeReady()) {
    ++waves;
    for (const auto& n : ready) {
      EXPECT_TRUE(g.IsStarting(n));
      EXPECT_TRUE(g.Busy());
      order.push_back(n);
    }
    for (const auto& n : ready) g.SetRunning(n, true);
  }
  EXPECT_FALSE(g.Busy());
  return {order, waves};
}

TEST(StartGraphTest, IndependentEntriesStartTogether) {
  StartGraph g{{{"stats", {}}, {"echo", {}}, {"monitor", {"stats"}}}};
  EXPECT_TRUE(g.Skipped().empty());
  for (const auto& n : g.Names()) g.Want(n);
  EXPECT_EQ(g.TakeReady(), (std::vector<std::string>{"stats", "echo"}));
  EXPECT_TRUE(g.TakeReady().empty()) << "monitor waits for stats";
  g.SetRunning("echo", true);
  EXPECT_TRUE(g.TakeReady().empty());
  g.SetRunning("stats", true);
  EXPECT_EQ(g.TakeReady(), std::vector<std::string>{"monitor"});
  EXPECT_EQ(g.Depth(), 2);
}

TEST(StartGraphTest, BadEntriesAreSkippedTheRestStarts) {
  StartGraph g{{{"a", {}},
                {"x", {"y"}},
                {"y", {"x"}},
                {"z", {"x", "a"}},
                {"self", {"self"}},
                {"lost", {"nowhere"}},
                {"a", {}}}};
  EXPECT_EQ(g.Names(), std::vector<std::string>{"a"});
  for (const char* n : {"x", "y", "z", "self", "lost", "a (again)"}) {
    EXPECT_TRUE(g.Skipped().contains(n)) << n;
  }
  EXPECT_EQ(g.Skipped().at("self"), "depends on itself");
  EXPECT_NE(g.Skipped().at("lost").find("isn't listed"), std::string::npos);
  g.Want("x");  // skipped: ignored
  EXPECT_FALSE(g.IsWanted("x"));
  const auto [order, waves] = StartAll(g);
  EXPECT_EQ(order, std::vector<std::string>{"a"});
}

TEST(StartGraphTest, ARestartWaitsForItsDependencies) {
  StartGraph g{{{"db", {}}, {"app", {"db"}}}};
  StartAll(g);
  g.SetRunning("db", false);  // db exited, so did app
  g.SetRunning("app", false);
  g.Want("app");
  EXPECT_TRUE(g.TakeReady().empty()) << "app waits for db again";
  g.Want("db");
  EXPECT_EQ(g.TakeReady(), std::vector<std::string>{"db"});
  g.SetRunning("db", true);
  EXPECT_EQ(g.TakeReady(), std::vector<std::string>{"app"});
  EXPECT_TRUE(g.DependenciesRun("app"));
}

TEST(StartGraphTest, AFailedStartLeavesDependentsWaiting) {
  StartGraph g{{{"db", {}}, {"app", {"db"}}}};
  for (const auto& n : g.Names()) g.Want(n);
  EXPECT_EQ(g.TakeReady(), std::vector<std::string>{"db"});
  g.SetRunning("db", false);  // the start failed
  EXPECT_FALSE(g.Busy());
  EXPECT_TRUE(g.TakeReady().empty());
  EXPECT_TRUE(g.IsWanted("app"));
  g.Unwant("app");
  EXPECT_FALSE(g.IsWanted("app"));
}

TEST(StartGraphTest, StopOrderIsDependentsFirst) {
  StartGraph g{{{"netd", {}},
                {"stats", {"netd"}},
                {"monitor", {"stats"}},
                {"echo", {}}}};
  const auto order = g.StopOrder();
  const auto pos = [&](const char* n) {
    return std::ranges::find(order, n) - order.begin();
  };
  EXPECT_EQ(order.size(), 4U);
  EXPECT_LT(pos("monitor"), pos("stats"));
  EXPECT_LT(pos("stats"), pos("netd"));
}

// The properties, against test-support's reference answers.
TEST(StartGraphProperty, StartsExactlyTheStartableDependenciesFirst) {
  aember::test::ForAll(500, [](Gen& gen) {
    const auto graph = aember::test::RandomGraph(gen);
    StartGraph g{Nodes(graph)};
    const auto good = aember::test::Startable(graph);
    EXPECT_EQ(std::set<std::string>(g.Names().begin(), g.Names().end()), good);
    EXPECT_EQ(g.Skipped().size(), graph.size() - good.size());
    const auto [order, waves] = StartAll(g);
    EXPECT_EQ(std::set<std::string>(order.begin(), order.end()), good);
    EXPECT_TRUE(aember::test::DependenciesFirst(order, graph));
    // Waves = the longest chain: everything independent starts together.
    EXPECT_EQ(waves, good.empty() ? 0 : aember::test::Depth(graph));
    EXPECT_EQ(g.Depth(), good.empty() ? 0 : aember::test::Depth(graph));
  });
}

TEST(StartGraphProperty, StopOrderReversesADependencyOrder) {
  aember::test::ForAll(300, [](Gen& gen) {
    const auto graph = aember::test::RandomGraph(gen);
    StartGraph g{Nodes(graph)};
    auto order = g.StopOrder();
    std::ranges::reverse(order);
    EXPECT_TRUE(aember::test::DependenciesFirst(order, graph));
    EXPECT_EQ(order.size(), g.Names().size());
  });
}

}  // namespace
