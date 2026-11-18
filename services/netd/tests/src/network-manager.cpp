/**
 * @file network-manager.cpp
 * @author Arian Ajdari
 * @brief NetworkManager tests without root: network.json parsing and the
 *        state before Start(). Bring-up needs root and real interfaces and
 *        is covered by the QEMU boots.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/netd/network-manager.h>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>

namespace {

using aember::netd::NetworkManager;
using nlohmann::json;
using namespace std::chrono_literals;

const json kValid = json::parse(R"({
  "ping_target": "1.1.1.1",
  "ping_retries": 2,
  "ping_interval_ms": 5000,
  "interfaces": [
    {"name": "eth0", "mode": "dhcp", "required": true},
    {"name": "eth1", "mode": "static", "address": "10.0.0.2/24",
     "gateway": "10.0.0.1", "dns": ["9.9.9.9", "1.1.1.1"]}
  ],
  "bridges": [{"name": "br0", "address": "10.0.3.1/24"}]
})");

TEST(NetworkManagerTest, ValidConfigConstructs) {
  EXPECT_NO_THROW({ NetworkManager nm{kValid}; });
}

TEST(NetworkManagerTest, OfflineBeforeStart) {
  NetworkManager nm{kValid};
  EXPECT_FALSE(nm.IsOnline());
  EXPECT_FALSE(nm.GetStatus().online);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(nm.WaitForConnectivity(200ms));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 200ms);
}

TEST(NetworkManagerTest, StopWithoutStartIsSafe) {
  NetworkManager nm{kValid};
  EXPECT_NO_THROW(nm.Stop());
  EXPECT_NO_THROW(nm.Stop());
}

// Bridges moved to containers.json (aember-containerd): an old network.json
// that still lists them is accepted, the entries are ignored.
TEST(NetworkManagerTest, OldBridgesEntryIsIgnored) {
  EXPECT_NO_THROW({
    NetworkManager nm{
        json::parse(R"({"interfaces": [], "bridges": [{"name": "br0"}]})")};
  });
}

TEST(NetworkManagerTest, MinimalConfigUsesDefaults) {
  EXPECT_NO_THROW({ NetworkManager nm{json::parse(R"({"interfaces": []})")}; });
  EXPECT_NO_THROW({
    NetworkManager nm{json::parse(R"({"interfaces": [{"name": "eth0"}]})")};
  });
}

// An invalid network.json throws from the constructor; netd exits with an
// error and PID 1 restarts it.
TEST(NetworkManagerTest, InvalidConfigsThrow) {
  for (const auto* text : {
           R"({})",
           R"({"interfaces": "eth0"})",
           R"({"interfaces": [{"mode": "dhcp"}]})",
           R"({"interfaces": [{"name": "eth0", "mode": "static"}]})",
       }) {
    EXPECT_THROW(
        { NetworkManager nm{json::parse(text)}; }, std::exception)
        << text;
  }
}

}  // namespace
