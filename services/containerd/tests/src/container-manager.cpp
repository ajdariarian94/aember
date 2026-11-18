/**
 * @file container-manager.cpp
 * @author Arian Ajdari
 * @brief ContainerManager tests that need neither root nor a container
 *        image: registry, config loading, exit handling, failed starts.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-services/containerd/container-manager.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::containerd::ContainerManager;
using State = ContainerManager::ContainerState;

class ContainerManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-cm-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
    cm_ = std::make_unique<ContainerManager>(
        std::make_shared<aember::mount_manager::MountManager>(),
        [this](const std::string& name, State, State to) {
          transitions_.emplace_back(name, to);
        });
  }
  void TearDown() override {
    cm_.reset();
    fs::remove_all(dir_);
  }

  /// A config without squashfs, so nothing gets mounted.
  ContainerManager::ContainerConfig Config(const std::string& name) const {
    ContainerManager::ContainerConfig cfg;
    cfg.name = name;
    cfg.rootfs = (dir_ / name).string();
    cfg.config_path = (dir_ / (name + ".conf")).string();
    return cfg;
  }

  fs::path dir_;
  std::unique_ptr<ContainerManager> cm_;
  std::vector<std::pair<std::string, State>> transitions_;
};

TEST_F(ContainerManagerTest, AddAndQuery) {
  ASSERT_TRUE(cm_->AddContainer(Config("echo")));
  EXPECT_TRUE(cm_->HasContainer("echo"));
  EXPECT_EQ(cm_->GetContainerState("echo"), State::kStopped);
  EXPECT_FALSE(cm_->IsRunning("echo"));
  EXPECT_FALSE(cm_->GetInitPid("echo").has_value());
}

TEST_F(ContainerManagerTest, RejectsDuplicatesAndIncompleteConfigs) {
  ASSERT_TRUE(cm_->AddContainer(Config("a")));
  EXPECT_FALSE(cm_->AddContainer(Config("a")));

  auto no_name = Config("x");
  no_name.name.clear();
  EXPECT_FALSE(cm_->AddContainer(no_name));

  auto no_rootfs = Config("y");
  no_rootfs.rootfs.clear();
  EXPECT_FALSE(cm_->AddContainer(no_rootfs));
  EXPECT_FALSE(cm_->HasContainer("y"));
}

TEST_F(ContainerManagerTest, UnknownNames) {
  EXPECT_FALSE(cm_->HasContainer("ghost"));
  EXPECT_EQ(cm_->GetContainerState("ghost"), State::kFailed);
  EXPECT_FALSE(cm_->StartContainer("ghost"));
  EXPECT_FALSE(cm_->StopContainer("ghost"));
  EXPECT_FALSE(cm_->RemoveContainer("ghost"));
}

TEST_F(ContainerManagerTest, Remove) {
  ASSERT_TRUE(cm_->AddContainer(Config("a")));
  EXPECT_TRUE(cm_->RemoveContainer("a"));
  EXPECT_FALSE(cm_->HasContainer("a"));
  EXPECT_TRUE(cm_->AddContainer(Config("a")));  // can be added again
}

TEST_F(ContainerManagerTest, StoppingAStoppedContainerIsANoOp) {
  ASSERT_TRUE(cm_->AddContainer(Config("a")));
  EXPECT_TRUE(cm_->StopContainer("a"));
  EXPECT_EQ(cm_->GetContainerState("a"), State::kStopped);
  EXPECT_TRUE(transitions_.empty());
}

TEST_F(ContainerManagerTest, HandleExitOfUnknownPidIsNotOurs) {
  ASSERT_TRUE(cm_->AddContainer(Config("a")));
  EXPECT_FALSE(cm_->HandleExit(999999, 0));
  EXPECT_EQ(cm_->GetContainerState("a"), State::kStopped);
}

// A container whose LXC config doesn't exist fails to start: the state goes
// starting -> failed and nothing throws.
TEST_F(ContainerManagerTest, BrokenContainerFailsCleanly) {
  ASSERT_TRUE(cm_->AddContainer(Config("broken")));
  bool ok = true;
  EXPECT_NO_THROW(ok = cm_->StartContainer("broken"));
  EXPECT_FALSE(ok);
  EXPECT_EQ(cm_->GetContainerState("broken"), State::kFailed);
  ASSERT_GE(transitions_.size(), 2u);
  EXPECT_EQ(transitions_.front().second, State::kStarting);
  EXPECT_EQ(transitions_.back().second, State::kFailed);
}

TEST_F(ContainerManagerTest, LoadContainersFromFile) {
  const auto path = dir_ / "containers.json";
  std::ofstream{path} << R"({"containers": [
    {"name": "one", "container": {"squashfs": "/s1", "rootfs": "/r1",
                                  "config_path": "/c1"}},
    {"name": "two", "container": {"squashfs": "/s2", "rootfs": "/r2",
                                  "config_path": "/c2", "args": ["x"]}}]})";
  const auto configs = cm_->LoadContainers(path.string());
  ASSERT_EQ(configs.size(), 2u);
  EXPECT_EQ(configs[0].name, "one");
  EXPECT_EQ(configs[1].args, (std::vector<std::string>{"x"}));
}

TEST_F(ContainerManagerTest, LoadContainersFromBadFileIsEmpty) {
  const auto path = dir_ / "bad.json";
  std::ofstream{path} << "{ not json";
  EXPECT_TRUE(cm_->LoadContainers(path.string()).empty());
  EXPECT_TRUE(cm_->LoadContainers("/nonexistent.json").empty());
}

}  // namespace

namespace {

using aember::containerd::ContainerManager;

TEST(ContainerExtraConfigTest, ParsesKeyValueLines) {
  const auto items = ContainerManager::ParseExtraConfig(
      "# comment\n"
      "\n"
      "lxc.environment = DISPLAY=:0\n"
      "   lxc.mount.entry  =  /tmp/.X11-unix tmp/.X11-unix none bind 0 0  \r\n"
      "no equals sign here\n"
      " = value without key\n"
      "lxc.environment=QT_QPA_PLATFORM=xcb");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].first, "lxc.environment");
  EXPECT_EQ(items[0].second, "DISPLAY=:0");  // only the first '=' splits
  EXPECT_EQ(items[1].first, "lxc.mount.entry");
  EXPECT_EQ(items[1].second, "/tmp/.X11-unix tmp/.X11-unix none bind 0 0");
  EXPECT_EQ(items[2].second, "QT_QPA_PLATFORM=xcb");  // no trailing newline
}

TEST(ContainerExtraConfigTest, EmptyAndCommentOnly) {
  EXPECT_TRUE(ContainerManager::ParseExtraConfig("").empty());
  EXPECT_TRUE(ContainerManager::ParseExtraConfig("# a\n  # b\n\n").empty());
}

// A missing extra-config file is fine: nothing is added and containers
// behave as before (the default path doesn't exist in the test env).
TEST_F(ContainerManagerTest, MissingExtraConfigFileIsIgnored) {
  cm_->SetExtraConfigPath((dir_ / "does-not-exist.conf").string());
  ASSERT_TRUE(cm_->AddContainer(Config("plain")));
  EXPECT_FALSE(cm_->StartContainer("plain"));  // fails on its own config
  EXPECT_EQ(cm_->GetContainerState("plain"), State::kFailed);
}

// Restart decisions after an exit. A container whose squashfs can't be
// mounted fails to start (kFailed) without root or LXC.
class ContainerRestartTest : public ContainerManagerTest {
 protected:
  ContainerManager::ContainerConfig Failing(
      const std::string& name, aember::containerd::RestartPolicy policy,
      unsigned max_attempts = 0) const {
    auto cfg = Config(name);
    cfg.squashfs = (dir_ / "missing.squashfs").string();
    cfg.restart_policy = policy;
    cfg.max_restart_attempts = max_attempts;
    cfg.restart_delay = std::chrono::seconds{3};
    return cfg;
  }
};

TEST_F(ContainerRestartTest, PolicyNoNeverRestarts) {
  ASSERT_TRUE(
      cm_->AddContainer(Failing("a", aember::containerd::RestartPolicy::kNo)));
  EXPECT_FALSE(cm_->StartContainer("a"));
  EXPECT_FALSE(cm_->NextRestart("a").has_value());
}

TEST_F(ContainerRestartTest, OnFailureAndAlwaysRestartAfterAFailure) {
  using aember::containerd::RestartPolicy;
  ASSERT_TRUE(cm_->AddContainer(Failing("f", RestartPolicy::kOnFailure)));
  ASSERT_TRUE(cm_->AddContainer(Failing("s", RestartPolicy::kAlways)));
  for (const char* name : {"f", "s"}) {
    EXPECT_FALSE(cm_->StartContainer(name));
    EXPECT_EQ(cm_->GetContainerState(name), State::kFailed);
    EXPECT_EQ(cm_->NextRestart(name), std::chrono::seconds{3}) << name;
  }
}

TEST_F(ContainerRestartTest, OnFailureLeavesACleanStopAlone) {
  using aember::containerd::RestartPolicy;
  ASSERT_TRUE(cm_->AddContainer(Failing("f", RestartPolicy::kOnFailure)));
  // Never started: kStopped, not a failure.
  EXPECT_FALSE(cm_->NextRestart("f").has_value());
}

TEST_F(ContainerRestartTest, GivesUpAfterMaxAttempts) {
  ASSERT_TRUE(cm_->AddContainer(
      Failing("m", aember::containerd::RestartPolicy::kAlways, 2)));
  for (int attempt = 1; attempt <= 2; ++attempt) {
    EXPECT_FALSE(cm_->StartContainer("m"));
    EXPECT_TRUE(cm_->NextRestart("m").has_value()) << attempt;
  }
  EXPECT_FALSE(cm_->StartContainer("m"));
  EXPECT_FALSE(cm_->NextRestart("m").has_value());
}

TEST_F(ContainerRestartTest, StopRequestedIsNeverRestarted) {
  ASSERT_TRUE(cm_->AddContainer(
      Failing("r", aember::containerd::RestartPolicy::kAlways)));
  EXPECT_FALSE(cm_->StartContainer("r"));  // kFailed
  // A failed container counts as stopped for StopContainer only once it was
  // running; force the request through the state it is in.
  EXPECT_TRUE(cm_->StopContainer("r"));
  EXPECT_FALSE(cm_->NextRestart("r").has_value());
  EXPECT_FALSE(cm_->NextRestart("unknown").has_value());
}

}  // namespace
