/**
 * @file container-parser.cpp
 * @author Arian Ajdari
 * @brief containers.json (names, dependencies), a package's config.json and
 * container state names.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/container-parser.h>
#include <aember-services/containerd/container-state.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::containerd::ContainerState;
using aember::containerd::ContainerStateToString;
using aember::containerd::LoadContainerConfig;
using aember::containerd::ParseBoardContainers;
using aember::containerd::ParseBridges;
using aember::containerd::ParseContainerConfig;
using nlohmann::json;

// ---------------------------------------------------------------------------
// containers.json: names and dependencies
// ---------------------------------------------------------------------------

TEST(BoardContainersTest, NamesAndDependencies) {
  const auto board = ParseBoardContainers(json::parse(R"({
    "allow_unsigned": true,
    "bridges": [{"name": "lxcbr0"}],
    "containers": [{"name": "stats"},
                   {"name": "monitor", "dependencies": ["stats"]}]})"));
  ASSERT_TRUE(board) << board.error();
  EXPECT_TRUE(board->allow_unsigned);
  ASSERT_EQ(board->containers.size(), 2U);
  EXPECT_EQ(board->containers[0].name, "stats");
  EXPECT_TRUE(board->containers[0].dependencies.empty());
  EXPECT_EQ(board->containers[1].dependencies,
            std::vector<std::string>{"stats"});
}

TEST(BoardContainersTest, ServicesAContainerNeeds) {
  const auto board = ParseBoardContainers(json::parse(R"({"containers": [
      {"name": "auth", "services": ["aember-netd"]}, {"name": "stats"}]})"));
  ASSERT_TRUE(board) << board.error();
  EXPECT_EQ(board->containers[0].services,
            std::vector<std::string>{"aember-netd"});
  EXPECT_TRUE(board->containers[1].services.empty());
}

TEST(BoardContainersTest, Defaults) {
  const auto board = ParseBoardContainers(json::object());
  ASSERT_TRUE(board);
  EXPECT_FALSE(board->allow_unsigned) << "signed packages only by default";
  EXPECT_TRUE(board->containers.empty());
}

TEST(BoardContainersTest, RejectsMalformed) {
  for (const char* text : {
           R"([])",
           R"({"allow_unsigned": "yes"})",
           R"({"containers": "x"})",
           R"({"containers": [1]})",
           R"({"containers": [{}]})",
           R"({"containers": [{"name": ""}]})",
           R"({"containers": [{"name": "../x"}]})",
           R"({"containers": [{"name": "Upper"}]})",
           R"({"containers": [{"name": "a", "dependencies": "b"}]})",
           R"({"containers": [{"name": "a", "dependencies": [1]}]})",
           R"({"containers": [{"name": "a", "services": "aember-netd"}]})",
           R"({"containers": [{"name": "a", "services": [true]}]})",
       }) {
    EXPECT_FALSE(ParseBoardContainers(json::parse(text))) << text;
  }
}

TEST(BoardContainersTest, TheOldFormatIsNamedInTheError) {
  const auto board = ParseBoardContainers(json::parse(R"({"containers": [
    {"name": "echo", "container": {"squashfs": "/x.squashfs"}}]})"));
  ASSERT_FALSE(board);
  EXPECT_NE(board.error().find("before .aember packages"), std::string::npos);
}

// ---------------------------------------------------------------------------
// config.json (in the package): how to start it
// ---------------------------------------------------------------------------

constexpr const char* kValid =
    R"({"name": "echo", "command": ["/bin/sh", "-c", "echo hi"]})";

json With(const char* extra) {
  auto j = json::parse(kValid);
  j.merge_patch(json::parse(extra));
  return j;
}

TEST(ContainerConfigTest, PathsComeFromTheName) {
  const auto c = ParseContainerConfig(json::parse(kValid), "/root");
  ASSERT_TRUE(c) << c.error();
  EXPECT_EQ(c->name, "echo");
  EXPECT_EQ(c->squashfs, "/root/echo/image.squashfs");
  EXPECT_EQ(c->config_path, "/root/echo/lxc.container.conf");
  EXPECT_EQ(c->rootfs, "/root/containers/echo");
  EXPECT_EQ(c->args, (std::vector<std::string>{"/bin/sh", "-c", "echo hi"}));
  const auto standard = ParseContainerConfig(json::parse(kValid));
  ASSERT_TRUE(standard);
  EXPECT_EQ(standard->squashfs, "/var/lib/aember/echo/image.squashfs");
}

TEST(ContainerConfigTest, LifecycleDefaults) {
  const auto c = ParseContainerConfig(json::parse(kValid));
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->init);
  EXPECT_EQ(c->stop_timeout, std::chrono::seconds{10});
  EXPECT_EQ(c->restart_policy, aember::containerd::RestartPolicy::kNo);
  EXPECT_EQ(c->max_restart_attempts, 0U);
  EXPECT_EQ(c->restart_delay, std::chrono::seconds{5});
}

TEST(ContainerConfigTest, LifecycleFields) {
  const auto c =
      ParseContainerConfig(With(R"({"init": false, "stop_timeout": 30,
               "restart_policy": "on-failure",
               "max_restart_attempts": 3, "restart_delay": 7})"));
  ASSERT_TRUE(c) << c.error();
  EXPECT_FALSE(c->init);
  EXPECT_EQ(c->stop_timeout, std::chrono::seconds{30});
  EXPECT_EQ(c->restart_policy, aember::containerd::RestartPolicy::kOnFailure);
  EXPECT_EQ(c->max_restart_attempts, 3U);
  EXPECT_EQ(c->restart_delay, std::chrono::seconds{7});
  for (const char* policy : {"no", "never", "always"}) {
    EXPECT_TRUE(ParseContainerConfig(With(
        (std::string{R"({"restart_policy": ")"} + policy + "\"}").c_str())))
        << policy;
  }
}

TEST(ContainerConfigTest, RejectsMissingOrWrongFields) {
  for (const char* text : {
           R"([])",
           R"({"command": ["/x"]})",
           R"({"name": "a"})",
           R"({"name": "a", "command": []})",
           R"({"name": "a", "command": "x"})",
           R"({"name": "a", "command": [1]})",
           R"({"name": "a/b", "command": ["/x"]})",
       }) {
    EXPECT_FALSE(ParseContainerConfig(json::parse(text))) << text;
  }
  for (const char* extra : {R"({"init": "yes"})",
                            R"({"stop_timeout": -1})",
                            R"({"stop_timeout": "10"})",
                            R"({"restart_policy": "sometimes"})",
                            R"({"restart_policy": 1})",
                            R"({"max_restart_attempts": -2})",
                            R"({"max_restart_attempts": 4294967296})",
                            R"({"restart_delay": 1.5})"}) {
    EXPECT_FALSE(ParseContainerConfig(With(extra))) << extra;
  }
}

TEST(ContainerConfigTest, LoadedFromTheInstalledPackage) {
  const auto root = fs::temp_directory_path() /
                    ("aember-container-config-" + std::to_string(::getpid()));
  fs::create_directories(root / "echo");
  std::ofstream{root / "echo" / "config.json"} << kValid;
  const auto c = LoadContainerConfig("echo", root.string());
  ASSERT_TRUE(c) << c.error();
  EXPECT_EQ(c->squashfs, (root / "echo" / "image.squashfs").string());
  EXPECT_FALSE(LoadContainerConfig("missing", root.string()));
  fs::create_directories(root / "other");
  std::ofstream{root / "other" / "config.json"} << kValid;  // names "echo"
  EXPECT_FALSE(LoadContainerConfig("other", root.string()));
  fs::remove_all(root);
}

TEST(ContainerStateTest, NamesAreDistinct) {
  std::set<std::string> names;
  for (const auto s : {ContainerState::kStopped,
                       ContainerState::kStarting,
                       ContainerState::kRunning,
                       ContainerState::kStopping,
                       ContainerState::kFailed}) {
    const auto name = ContainerStateToString(s);
    EXPECT_NE(name, "unknown");
    names.insert(name);
  }
  EXPECT_EQ(names.size(), 5u);
  EXPECT_EQ(ContainerStateToString(ContainerState::kRunning), "running");
}

// Regression: containerd crashed (uncaught type_error from json::value) on a
// "bridges" entry that wasn't an object or had a non-string name/address.
TEST(ParseBridgesTest, BadEntriesAreSkippedNeverThrown) {
  const auto config = nlohmann::json::parse(R"({"bridges": [
      "lxcbr0", 7, null, {"name": 1}, {"name": ""}, {"address": "10.0.0.1/24"},
      {"name": "br1", "address": 5},
      {"name": "br0", "address": "10.0.3.1/24"}, {"name": "br2"}]})");
  std::vector<std::string> errors;
  std::vector<aember::containerd::BridgeConfig> bridges;
  EXPECT_NO_THROW(bridges = ParseBridges(config, errors));
  ASSERT_EQ(bridges.size(), 2U);
  EXPECT_EQ(bridges[0].name, "br0");
  EXPECT_EQ(bridges[0].address, "10.0.3.1/24");
  EXPECT_EQ(bridges[1].name, "br2");
  EXPECT_EQ(bridges[1].address, "");
  EXPECT_EQ(errors.size(), 7U);
}

TEST(ParseBridgesTest, MissingOrWrongList) {
  std::vector<std::string> errors;
  EXPECT_TRUE(ParseBridges(nlohmann::json::object(), errors).empty());
  EXPECT_TRUE(ParseBridges(nlohmann::json::array(), errors).empty());
  EXPECT_TRUE(errors.empty());
  EXPECT_TRUE(ParseBridges({{"bridges", "lxcbr0"}}, errors).empty());
  EXPECT_EQ(errors.size(), 1U);
}

}  // namespace
