/**
 * @file daemon-status.cpp
 * @author Arian Ajdari
 * @brief ApplyDaemonMessage tests: what PID 1 keeps from its daemons.
 * @version 0.1
 * @date 2026-11-25
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/aember-init/daemon-status.h>

#include <gtest/gtest.h>

namespace {

using aember::aember_init::ApplyDaemonMessage;
using aember::aember_init::DaemonStatus;
using Kind = aember::aember_init::DaemonUpdate::Kind;
using nlohmann::json;

TEST(DaemonStatusTest, Ready) {
  DaemonStatus s;
  EXPECT_EQ(ApplyDaemonMessage({{"type", "ready"}}, s).kind, Kind::kReady);
}

TEST(DaemonStatusTest, NetworkReportIsKeptAndLossNoticed) {
  DaemonStatus s;
  const auto up = json{{"type", "event"},
                       {"name", "network"},
                       {"data", {{"online", true}, {"interface", "eth0"}}}};
  auto u = ApplyDaemonMessage(up, s);
  EXPECT_EQ(u.kind, Kind::kNetwork);
  EXPECT_FALSE(u.went_offline);
  EXPECT_EQ(s.network["interface"], "eth0");
  auto down = up;
  down["data"]["online"] = false;
  u = ApplyDaemonMessage(down, s);
  EXPECT_TRUE(u.went_offline);
  EXPECT_FALSE(ApplyDaemonMessage(down, s).went_offline);  // only once
}

TEST(DaemonStatusTest, ContainerStates) {
  DaemonStatus s;
  const auto u =
      ApplyDaemonMessage({{"type", "event"},
                          {"name", "container"},
                          {"data", {{"name", "web"}, {"state", "running"}}}},
                         s);
  EXPECT_EQ(u.kind, Kind::kContainer);
  EXPECT_EQ(s.containers.at("web"), "running");
}

TEST(DaemonStatusTest, AnythingElseIsIgnoredNeverThrown) {
  DaemonStatus s;
  for (const auto& m :
       {json(nullptr),
        json::array({1, 2}),
        json{{"type", 1}},
        json{{"type", "event"}},
        json{{"type", "event"}, {"name", "network"}, {"data", "up"}},
        json{{"type", "event"}, {"name", 5}, {"data", json::object()}},
        json{{"type", "event"},
             {"name", "container"},
             {"data", {{"name", 3}, {"state", "running"}}}},
        json{{"type", "event"}, {"name", "container"}, {"data", {{"x", 1}}}},
        json{{"type", "event"}, {"name", "other"}, {"data", json::object()}}}) {
    EXPECT_EQ(ApplyDaemonMessage(m, s).kind, Kind::kIgnored) << m.dump();
  }
  EXPECT_TRUE(s.network.is_null());
  EXPECT_TRUE(s.containers.empty());
}

}  // namespace
