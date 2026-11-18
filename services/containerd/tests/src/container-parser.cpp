/**
 * @file container-parser.cpp
 * @author Arian Ajdari
 * @brief ContainersConfigParser tests (containers.json) and container
 *        state names.
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
using aember::containerd::ContainersConfigParser;
using aember::containerd::ContainerState;
using aember::containerd::ContainerStateToString;
using aember::utils::config::ConfigError;

class ContainerParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-container-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  bool Parse(const std::string& json) {
    const auto path = dir_ / "containers.json";
    std::ofstream{path} << json;
    error_ = {};
    bool ok = false;
    EXPECT_NO_THROW(ok = parser_.ParseFile(path.string(), error_));
    return ok;
  }

  bool ParseOne(const std::string& entry) {
    return Parse(R"({"containers": [)" + entry + "]}");
  }

  fs::path dir_;
  ContainersConfigParser parser_;
  ConfigError error_;
};

constexpr const char* kValid = R"({
  "name": "echo",
  "container": {
    "squashfs": "/var/lib/aember/echo/echo.squashfs",
    "rootfs": "/var/lib/aember/containers/echo",
    "config_path": "/var/lib/aember/echo/lxc.container.conf",
    "args": ["/bin/sh", "-c", "echo hi"]
  }
})";

TEST_F(ContainerParserTest, FullEntry) {
  ASSERT_TRUE(ParseOne(kValid)) << error_.message;
  const auto& cs = parser_.GetContainers();
  ASSERT_EQ(cs.size(), 1u);
  EXPECT_EQ(cs[0].name, "echo");
  EXPECT_EQ(cs[0].squashfs, "/var/lib/aember/echo/echo.squashfs");
  EXPECT_EQ(cs[0].rootfs, "/var/lib/aember/containers/echo");
  EXPECT_EQ(cs[0].config_path, "/var/lib/aember/echo/lxc.container.conf");
  EXPECT_EQ(cs[0].args, (std::vector<std::string>{"/bin/sh", "-c", "echo hi"}));
}

TEST_F(ContainerParserTest, ArgsAreOptional) {
  ASSERT_TRUE(ParseOne(R"({"name": "a", "container": {
    "squashfs": "/s", "rootfs": "/r", "config_path": "/c"}})"));
  EXPECT_TRUE(parser_.GetContainers().at(0).args.empty());
}

TEST_F(ContainerParserTest, EmptyListIsValid) {
  EXPECT_TRUE(Parse(R"({"containers": []})"));
  EXPECT_TRUE(parser_.GetContainers().empty());
}

TEST_F(ContainerParserTest, RejectsBadFiles) {
  EXPECT_FALSE(Parse(""));
  EXPECT_FALSE(Parse("not json"));
  EXPECT_FALSE(Parse("[]"));
  EXPECT_FALSE(Parse(R"({"processes": []})"));
  EXPECT_FALSE(Parse(R"({"containers": "echo"})"));
  ConfigError error;
  EXPECT_FALSE(parser_.ParseFile("/nonexistent/containers.json", error));
}

TEST_F(ContainerParserTest, RejectsMissingOrWrongFields) {
  for (
      const auto* entry : {
          R"({"container": {"squashfs": "/s", "rootfs": "/r", "config_path": "/c"}})",
          R"({"name": "a"})",
          R"({"name": "a", "container": "x"})",
          R"({"name": "a", "container": {"rootfs": "/r", "config_path": "/c"}})",
          R"({"name": "a", "container": {"squashfs": "/s", "config_path": "/c"}})",
          R"({"name": "a", "container": {"squashfs": "/s", "rootfs": "/r"}})",
          R"({"name": "a", "container": {"squashfs": 1, "rootfs": "/r", "config_path": "/c"}})",
          R"({"name": "a", "container": {"squashfs": "/s", "rootfs": "/r", "config_path": "/c", "args": "x"}})",
          R"({"name": "a", "container": {"squashfs": "/s", "rootfs": "/r", "config_path": "/c", "args": [1]}})",
      }) {
    EXPECT_FALSE(ParseOne(entry)) << entry;
    EXPECT_FALSE(error_.message.empty()) << entry;
  }
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

TEST_F(ContainerParserTest, LifecycleDefaults) {
  ASSERT_TRUE(ParseOne(kValid)) << error_.message;
  const auto& c = parser_.GetContainers().at(0);
  EXPECT_TRUE(c.init);
  EXPECT_EQ(c.stop_timeout, std::chrono::seconds{10});
  EXPECT_EQ(c.restart_policy, aember::containerd::RestartPolicy::kNo);
  EXPECT_EQ(c.max_restart_attempts, 0u);
  EXPECT_EQ(c.restart_delay, std::chrono::seconds{5});
}

TEST_F(ContainerParserTest, LifecycleFields) {
  std::string entry = kValid;
  entry.insert(entry.rfind('}'),
               R"(, "init": false, "stop_timeout": 30,
                  "restart_policy": "on-failure",
                  "max_restart_attempts": 3, "restart_delay": 7)");
  ASSERT_TRUE(ParseOne(entry)) << error_.message;
  const auto& c = parser_.GetContainers().at(0);
  EXPECT_FALSE(c.init);
  EXPECT_EQ(c.stop_timeout, std::chrono::seconds{30});
  EXPECT_EQ(c.restart_policy, aember::containerd::RestartPolicy::kOnFailure);
  EXPECT_EQ(c.max_restart_attempts, 3u);
  EXPECT_EQ(c.restart_delay, std::chrono::seconds{7});

  for (const char* policy : {"no", "never", "always"}) {
    std::string e = kValid;
    e.insert(e.rfind('}'),
             std::string{R"(, "restart_policy": ")"} + policy + "\"");
    EXPECT_TRUE(ParseOne(e)) << policy << ": " << error_.message;
  }
}

TEST_F(ContainerParserTest, BadLifecycleFieldsAreRejected) {
  for (const char* extra : {R"(, "init": "yes")",
                            R"(, "stop_timeout": -1)",
                            R"(, "stop_timeout": "10")",
                            R"(, "restart_policy": "sometimes")",
                            R"(, "restart_policy": 1)",
                            R"(, "max_restart_attempts": -2)",
                            R"(, "restart_delay": 1.5)"}) {
    std::string e = kValid;
    e.insert(e.rfind('}'), extra);
    EXPECT_FALSE(ParseOne(e)) << extra;
  }
}

}  // namespace
