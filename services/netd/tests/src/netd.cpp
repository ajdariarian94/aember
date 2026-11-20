/**
 * @file netd.cpp
 * @author Arian Ajdari
 * @brief The netd daemon end to end (RunNetd in a forked child, a channel
 *        on fd 3 like PID 1 gives it): readiness, status reports, control
 *        messages, signals, configuration errors.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/netlink/netlink.h>
#include <aember-services/netd/netd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <sys/mount.h>
#include <unistd.h>

#include "../../../tests-common/daemon-runner.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using aember_test::DaemonRunner;

class NetdDaemonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tmpl =
        (fs::temp_directory_path() / "netd-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
    dir_ = tmpl;
    options_.network_json = (dir_ / "network.json").string();
    options_.log_file = (dir_ / "netd.log").string();
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void Config(const nlohmann::json& config) {
    std::ofstream{options_.network_json} << config.dump();
  }

  std::string Log() const {
    std::ifstream file{options_.log_file};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
  }

  DaemonRunner Run() {
    const auto options = options_;
    return DaemonRunner{[options] { return aember::netd::RunNetd(options); }};
  }

  fs::path dir_;
  aember::netd::NetdOptions options_;
};

TEST_F(NetdDaemonTest, MissingConfigExitsWithOneAndNeverReportsReady) {
  auto netd = Run();  // no network.json
  ASSERT_TRUE(netd.Started());
  EXPECT_EQ(netd.Wait(10s), std::optional<int>{1});
  EXPECT_FALSE(netd.Expect("ready", 200ms));
}

TEST_F(NetdDaemonTest, ConfigWithoutInterfacesArrayFailsBringUp) {
  Config({{"ping_target", "127.0.0.1"}});
  auto netd = Run();
  EXPECT_EQ(netd.Wait(10s), std::optional<int>{1});
  EXPECT_NE(Log().find("network bring-up failed"), std::string::npos);
}

TEST_F(NetdDaemonTest, MissingRequiredInterfaceFailsBringUp) {
  Config(
      {{"interfaces",
        nlohmann::json::array(
            {{{"name", "aemnone0"}, {"mode", "dhcp"}, {"required", true}}})}});
  auto netd = Run();
  EXPECT_EQ(netd.Wait(10s), std::optional<int>{1});
  EXPECT_NE(Log().find("aemnone0"), std::string::npos);
}

TEST_F(NetdDaemonTest, ReadyThenStopsOnSigterm) {
  Config({{"interfaces", nlohmann::json::array()},
          {"ping_target", "127.0.0.1"},
          {"ping_interval_ms", 200}});
  auto netd = Run();
  ASSERT_TRUE(netd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(netd.Stop(SIGTERM, 10s), std::optional<int>{0});
  EXPECT_NE(Log().find("netd ready"), std::string::npos);
  EXPECT_NE(Log().find("netd stopping"), std::string::npos);
}

TEST_F(NetdDaemonTest, SighupAndConsoleMessagesDontStopIt) {
  Config({{"interfaces", nlohmann::json::array()},
          {"ping_target", "127.0.0.1"},
          {"ping_interval_ms", 200}});
  auto netd = Run();
  ASSERT_TRUE(netd.Expect("ready", 10s));
  // A debug shell opens and closes (PID 1 mutes and unmutes the console).
  ASSERT_TRUE(netd.Channel().Send(aember::utils::ipc::Console(false)));
  ASSERT_TRUE(netd.Channel().Send(aember::utils::ipc::Console(true)));
  ::kill(netd.Pid(), SIGHUP);
  EXPECT_FALSE(netd.Wait(500ms)) << "netd exited on SIGHUP";
  EXPECT_EQ(netd.Stop(SIGINT, 10s), std::optional<int>{0});
}

TEST_F(NetdDaemonTest, OptionalMissingInterfaceIsSkipped) {
  Config(
      {{"interfaces",
        nlohmann::json::array(
            {{{"name", "aemnone0"}, {"mode", "dhcp"}, {"required", false}}})},
       {"dhcp_timeout_ms", 200},
       {"dhcp_retries", 1}});
  auto netd = Run();
  ASSERT_TRUE(netd.Expect("ready", 15s)) << Log();
  EXPECT_EQ(netd.Stop(SIGTERM, 10s), std::optional<int>{0});
}

// ---------------------------------------------------------------------------
// As root: a veth pair in netd's private network namespace (DaemonRunner),
// with a temp file over /etc/resolv.conf (a mount namespace doesn't stop
// writes to the real file, a bind mount over it does).
// ---------------------------------------------------------------------------

/// The daemon body for root tests: veth aemt0/aemt1, a fake resolv.conf,
/// then RunNetd.
int RunNetdOnAVeth(const aember::netd::NetdOptions& options,
                   const std::string& resolv) {
  namespace netlink = aember::utils::netlink;
  if (netlink::CreateVethPair("aemt0", "aemt1") != 0) return 50;
  { std::ofstream{resolv}; }
  if (::mount(resolv.c_str(), "/etc/resolv.conf", nullptr, MS_BIND, nullptr) !=
      0) {
    return 51;
  }
  return aember::netd::RunNetd(options);
}

TEST_F(NetdDaemonTest, StaticAddressRouteAndDnsAreConfiguredAndReported) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  Config({{"interfaces",
           nlohmann::json::array({{{"name", "aemt0"},
                                   {"mode", "static"},
                                   {"address", "10.77.0.2/24"},
                                   {"gateway", "10.77.0.1"},
                                   {"dns", {"10.77.0.53"}},
                                   {"required", true}}})},
          {"ping_target", "10.77.0.1"},
          {"ping_interval_ms", 200},
          {"ping_retries", 1}});
  const auto options = options_;
  const auto resolv = (dir_ / "resolv.conf").string();
  DaemonRunner netd{
      [options, resolv] { return RunNetdOnAVeth(options, resolv); }};
  ASSERT_TRUE(netd.Expect("ready", 15s)) << Log();
  // The status report names the interface and its address (GetNetworkInfo).
  bool seen = false;
  for (int i = 0; i < 20 && !seen; ++i) {
    const auto event = netd.Expect("event", 2s);
    if (!event) continue;
    const auto& data = (*event)["data"];
    seen = data.value("interface", "") == "aemt0" &&
           data.value("ip", "") == "10.77.0.2";
  }
  const auto pid_netns =
      std::filesystem::path{"/proc"} / std::to_string(netd.Pid()) / "net";
  std::ifstream route{pid_netns / "route"};
  const std::string routes{std::istreambuf_iterator<char>{route}, {}};
  EXPECT_EQ(netd.Stop(SIGTERM, 10s), std::optional<int>{0});
  EXPECT_TRUE(seen) << Log();
  // Default route via 10.77.0.1 (0100 4D0A, little endian) on aemt0.
  EXPECT_NE(routes.find("aemt0\t00000000\t01004D0A"), std::string::npos)
      << routes;
  std::ifstream resolv_in{resolv};
  const std::string dns{std::istreambuf_iterator<char>{resolv_in}, {}};
  EXPECT_NE(dns.find("nameserver 10.77.0.53"), std::string::npos) << dns;
}

TEST_F(NetdDaemonTest, InvalidStaticAddressFailsARequiredInterface) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root";
  Config({{"interfaces",
           nlohmann::json::array({{{"name", "aemt0"},
                                   {"mode", "static"},
                                   {"address", "10.77.0.300/24"},
                                   {"required", true}}})}});
  const auto options = options_;
  const auto resolv = (dir_ / "resolv.conf").string();
  DaemonRunner netd{
      [options, resolv] { return RunNetdOnAVeth(options, resolv); }};
  EXPECT_EQ(netd.Wait(10s), std::optional<int>{1});
  EXPECT_NE(Log().find("invalid CIDR"), std::string::npos) << Log();
}

}  // namespace
