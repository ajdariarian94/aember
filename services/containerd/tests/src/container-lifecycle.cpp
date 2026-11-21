/**
 * @file container-lifecycle.cpp
 * @author Arian Ajdari
 * @brief ContainerManager against a fake liblxc (SetLxcForTesting): start
 *        with and without the init wrapper, graceful and forced stop, a
 *        stop that fails, adopting a running container, exits and
 *        restarts. No root, no mounts (configs without squashfs), no real
 *        containers: the fake's "init" is a forked `sleep`.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-services/containerd/container-manager.h>
#include <aember-services/containerd/init-wrapper.h>
#include <aember-services/containerd/lxc-api.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <lxc/lxccontainer.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::containerd::ContainerManager;
using State = ContainerManager::ContainerState;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Fake liblxc: one Fake per container name, shared by every handle to it
// (like LXC's on-disk state). Its init is a real child process.
// ---------------------------------------------------------------------------

struct Fake {
  bool load_ok = true;
  bool start_ok = true;
  bool shutdown_ok = true;
  bool stop_ok = true;
  pid_t init = -1;
  std::vector<std::string> argv;
  std::map<std::string, std::string> config;
  std::string loaded_config;
  int shutdown_timeout = -1;
  int shutdowns = 0;
  int stops = 0;
  int starts = 0;
  int handles = 0;
};

std::map<std::string, Fake>& Fakes() {
  static std::map<std::string, Fake> fakes;
  return fakes;
}

Fake& Of(lxc_container* c) {
  return Fakes()[c->name];
}

void KillInit(Fake& f) {
  if (f.init > 0) {
    ::kill(f.init, SIGKILL);
    ::waitpid(f.init, nullptr, 0);
    f.init = -1;
  }
}

bool FakeIsRunning(lxc_container* c) {
  const auto& f = Of(c);
  return f.init > 0 && ::kill(f.init, 0) == 0;
}
pid_t FakeInitPid(lxc_container* c) {
  return FakeIsRunning(c) ? Of(c).init : -1;
}
bool FakeLoadConfig(lxc_container* c, const char* file) {
  Of(c).loaded_config = file ? file : "";
  return Of(c).load_ok;
}
bool FakeSetConfigItem(lxc_container* c, const char* key, const char* value) {
  Of(c).config[key] = value;
  return true;
}
bool FakeStart(lxc_container* c, int /*useinit*/, char* const argv[]) {
  auto& f = Of(c);
  ++f.starts;
  f.argv.clear();
  for (int i = 0; argv != nullptr && argv[i] != nullptr; ++i) {
    f.argv.emplace_back(argv[i]);
  }
  if (!f.start_ok) return false;
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execlp("sleep", "sleep", "600", nullptr);
    ::_exit(127);
  }
  f.init = pid;
  return pid > 0;
}
bool FakeShutdown(lxc_container* c, int timeout) {
  auto& f = Of(c);
  ++f.shutdowns;
  f.shutdown_timeout = timeout;
  if (!f.shutdown_ok) return false;
  KillInit(f);
  return true;
}
bool FakeStop(lxc_container* c) {
  auto& f = Of(c);
  ++f.stops;
  if (!f.stop_ok) return false;
  KillInit(f);
  return true;
}

lxc_container* FakeNew(const char* name, const char* /*configpath*/) {
  auto* c = new lxc_container{};
  c->name = ::strdup(name);
  c->is_running = FakeIsRunning;
  c->init_pid = FakeInitPid;
  c->load_config = FakeLoadConfig;
  c->set_config_item = FakeSetConfigItem;
  c->start = FakeStart;
  c->shutdown = FakeShutdown;
  c->stop = FakeStop;
  ++Fakes()[name].handles;
  return c;
}
int FakePut(lxc_container* c) {
  --Fakes()[c->name].handles;
  std::free(c->name);
  delete c;
  return 1;
}

const aember::containerd::LxcApi kFakeApi{FakeNew, FakePut};

class ContainerLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    aember::containerd::SetLxcForTesting(&kFakeApi);
    Fakes().clear();
    dir_ = fs::temp_directory_path() /
           ("aember-lifecycle-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
    cm_ = std::make_unique<ContainerManager>(
        std::make_shared<aember::mount_manager::MountManager>(),
        [this](const std::string& name, State, State to) {
          transitions_.emplace_back(name, to);
        });
  }
  void TearDown() override {
    cm_.reset();
    for (auto& [name, f] : Fakes()) KillInit(f);
    aember::containerd::SetLxcForTesting(nullptr);
    fs::remove_all(dir_);
  }

  /// No squashfs: Create() mounts nothing and goes straight to LXC.
  ContainerManager::ContainerConfig Config(const std::string& name,
                                           bool with_wrapper = false) {
    ContainerManager::ContainerConfig cfg;
    cfg.name = name;
    cfg.rootfs = (dir_ / name).string();
    cfg.config_path = (dir_ / (name + ".conf")).string();
    cfg.args = {"/usr/bin/app", "--flag"};
    cfg.init = with_wrapper;
    fs::create_directories(cfg.rootfs);
    if (with_wrapper) {
      // HasInitWrapper: a non-empty /busybox.aember-init in the rootfs.
      std::ofstream{cfg.rootfs + aember::containerd::kInitInContainer}
          << "busybox";
    }
    return cfg;
  }

  bool Saw(const std::string& name, State state) const {
    for (const auto& [n, s] : transitions_) {
      if (n == name && s == state) return true;
    }
    return false;
  }

  fs::path dir_;
  std::unique_ptr<ContainerManager> cm_;
  std::vector<std::pair<std::string, State>> transitions_;
};

TEST_F(ContainerLifecycleTest, StartRunsTheAppDirectlyWithoutTheWrapper) {
  ASSERT_TRUE(cm_->AddContainer(Config("plain")));
  ASSERT_TRUE(cm_->StartContainer("plain"));
  EXPECT_TRUE(cm_->IsRunning("plain"));
  EXPECT_EQ(cm_->GetContainerState("plain"), State::kRunning);
  const auto& f = Fakes()["plain"];
  EXPECT_EQ(f.argv, (std::vector<std::string>{"/usr/bin/app", "--flag"}));
  EXPECT_EQ(f.loaded_config, (dir_ / "plain.conf").string());
  EXPECT_FALSE(f.config.contains("lxc.signal.halt"));
  ASSERT_TRUE(cm_->GetInitPid("plain").has_value());
  EXPECT_EQ(*cm_->GetInitPid("plain"), f.init);
}

TEST_F(ContainerLifecycleTest, StartRunsTheAppUnderTheInitWrapper) {
  ASSERT_TRUE(cm_->AddContainer(Config("wrapped", true)));
  ASSERT_TRUE(cm_->StartContainer("wrapped"));
  const auto& f = Fakes()["wrapped"];
  ASSERT_GE(f.argv.size(), 7u);
  EXPECT_EQ(f.argv[0], aember::containerd::kInitInContainer);
  EXPECT_EQ(f.argv[1], "sh");
  EXPECT_EQ(f.argv[2], "-c");
  EXPECT_EQ(f.argv[3], aember::containerd::kInitScript);
  EXPECT_EQ(f.argv[5], "/usr/bin/app");
  // `lxc shutdown` must send SIGTERM, which the wrapper passes on.
  EXPECT_EQ(f.config.at("lxc.signal.halt"), "SIGTERM");
}

TEST_F(ContainerLifecycleTest, StopIsGracefulWithTheConfiguredTimeout) {
  auto cfg = Config("graceful");
  cfg.stop_timeout = 7s;
  ASSERT_TRUE(cm_->AddContainer(cfg));
  ASSERT_TRUE(cm_->StartContainer("graceful"));
  ASSERT_TRUE(cm_->StopContainer("graceful"));
  const auto& f = Fakes()["graceful"];
  EXPECT_EQ(f.shutdowns, 1);
  EXPECT_EQ(f.shutdown_timeout, 7);
  EXPECT_EQ(f.stops, 0);
  EXPECT_EQ(cm_->GetContainerState("graceful"), State::kStopped);
  EXPECT_EQ(f.handles, 0) << "the LXC handle was not released";
}

TEST_F(ContainerLifecycleTest, StopIsForcedWhenShutdownFails) {
  ASSERT_TRUE(cm_->AddContainer(Config("forced")));
  ASSERT_TRUE(cm_->StartContainer("forced"));
  Fakes()["forced"].shutdown_ok = false;
  ASSERT_TRUE(cm_->StopContainer("forced"));
  EXPECT_EQ(Fakes()["forced"].stops, 1);
  EXPECT_EQ(cm_->GetContainerState("forced"), State::kStopped);
}

// Regression: a container that survives both shutdown and stop was reported
// as stopped (Stop() always returned true).
TEST_F(ContainerLifecycleTest, StopThatFailsIsReportedAsFailed) {
  ASSERT_TRUE(cm_->AddContainer(Config("stuck")));
  ASSERT_TRUE(cm_->StartContainer("stuck"));
  Fakes()["stuck"].shutdown_ok = false;
  Fakes()["stuck"].stop_ok = false;
  EXPECT_FALSE(cm_->StopContainer("stuck"));
  EXPECT_EQ(cm_->GetContainerState("stuck"), State::kFailed);
}

TEST_F(ContainerLifecycleTest, ConfigThatDoesNotLoadFailsTheStart) {
  ASSERT_TRUE(cm_->AddContainer(Config("badconf")));
  Fakes()["badconf"].load_ok = false;
  EXPECT_FALSE(cm_->StartContainer("badconf"));
  EXPECT_FALSE(cm_->IsRunning("badconf"));
  EXPECT_EQ(Fakes()["badconf"].starts, 0);
}

TEST_F(ContainerLifecycleTest, StartThatFailsIsReported) {
  ASSERT_TRUE(cm_->AddContainer(Config("nostart")));
  Fakes()["nostart"].start_ok = false;
  EXPECT_FALSE(cm_->StartContainer("nostart"));
  EXPECT_EQ(cm_->GetContainerState("nostart"), State::kFailed);
}

TEST_F(ContainerLifecycleTest, AdoptsAContainerThatIsStillRunning) {
  ASSERT_TRUE(cm_->AddContainer(Config("left")));
  // A previous containerd started it; it kept running (LXC's state).
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execlp("sleep", "sleep", "600", nullptr);
    ::_exit(127);
  }
  Fakes()["left"].init = pid;
  ASSERT_TRUE(cm_->Adopt("left"));
  EXPECT_TRUE(cm_->IsRunning("left"));
  EXPECT_EQ(cm_->GetInitPid("left"), std::optional<pid_t>{pid});
  EXPECT_EQ(Fakes()["left"].starts, 0);  // adopted, not started again
  EXPECT_FALSE(cm_->Adopt("left"));      // already ours
  EXPECT_FALSE(cm_->Adopt("unknown"));
}

TEST_F(ContainerLifecycleTest, NothingToAdoptWhenItIsNotRunning) {
  ASSERT_TRUE(cm_->AddContainer(Config("gone")));
  EXPECT_FALSE(cm_->Adopt("gone"));
  EXPECT_EQ(Fakes()["gone"].handles, 0);
}

TEST_F(ContainerLifecycleTest, ExitOfTheInitIsHandledAndRestarted) {
  auto cfg = Config("crashy");
  cfg.restart_policy = aember::containerd::RestartPolicy::kAlways;
  cfg.restart_delay = 1s;
  ASSERT_TRUE(cm_->AddContainer(cfg));
  ASSERT_TRUE(cm_->StartContainer("crashy"));
  const pid_t init = Fakes()["crashy"].init;
  KillInit(Fakes()["crashy"]);  // the app died
  EXPECT_TRUE(cm_->HandleExit(init, 1));
  EXPECT_FALSE(cm_->IsRunning("crashy"));
  EXPECT_EQ(cm_->NextRestart("crashy"),
            std::optional<std::chrono::seconds>{1s});
  // containerd's loop restarts it after the delay.
  ASSERT_TRUE(cm_->StartContainer("crashy"));
  EXPECT_EQ(Fakes()["crashy"].starts, 2);
  EXPECT_TRUE(cm_->IsRunning("crashy"));
}

TEST_F(ContainerLifecycleTest, StopAllStopsEveryRunningContainer) {
  ASSERT_TRUE(cm_->AddContainer(Config("a")));
  ASSERT_TRUE(cm_->AddContainer(Config("b")));
  ASSERT_TRUE(cm_->StartContainer("a"));
  ASSERT_TRUE(cm_->StartContainer("b"));
  cm_->StopAll();
  EXPECT_FALSE(cm_->IsRunning("a"));
  EXPECT_FALSE(cm_->IsRunning("b"));
  EXPECT_TRUE(Saw("a", State::kStopped));
  EXPECT_TRUE(Saw("b", State::kStopped));
}

TEST_F(ContainerLifecycleTest, StartingARunningContainerIsANoOp) {
  ASSERT_TRUE(cm_->AddContainer(Config("once")));
  ASSERT_TRUE(cm_->StartContainer("once"));
  EXPECT_TRUE(cm_->StartContainer("once"));
  EXPECT_EQ(Fakes()["once"].starts, 1);
}

}  // namespace
