/**
 * @file containerd.cpp
 * @author Arian Ajdari
 * @brief The containerd daemon end to end (RunContainerd in a forked child
 *        with a channel on fd 3): readiness, bridges (root, private network
 *        namespace), a container that can't start being retried and
 *        reported, signals and control messages.
 * @version 0.1
 * @date 2026-11-19
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/package/writer.h>
#include <aember-services/containerd/containerd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "../../../tests-common/daemon-runner.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using aember_test::DaemonRunner;

std::string ReadFile(const fs::path& path) {
  std::ifstream file{path};
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

class ContainerdDaemonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tmpl =
        (fs::temp_directory_path() / "containerd-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
    dir_ = tmpl;
    options_.containers_json = (dir_ / "containers.json").string();
    options_.log_file = (dir_ / "containerd.log").string();
    options_.packages_root = (dir_ / "lib").string();
    options_.inbox = (dir_ / "lib" / "packages").string();
    options_.keys_dir = (dir_ / "keys").string();
    options_.control_socket.clear();
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void Config(const nlohmann::json& config) {
    std::ofstream{options_.containers_json} << config.dump();
  }
  std::string Log() const { return ReadFile(options_.log_file); }

  DaemonRunner Run() {
    const auto options = options_;
    return DaemonRunner{
        [options] { return aember::containerd::RunContainerd(options); }};
  }

  fs::path dir_;
  aember::containerd::ContainerdOptions options_;
};

TEST_F(ContainerdDaemonTest, NoContainersReadyThenStopsOnSigterm) {
  Config({{"containers", nlohmann::json::array()}});
  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
  EXPECT_NE(Log().find("containerd ready (0 container(s))"), std::string::npos);
}

TEST_F(ContainerdDaemonTest, MissingConfigStillReportsReady) {
  auto ctrd = Run();  // no containers.json: nothing to run, but up
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(ctrd.Stop(SIGINT, 15s), std::optional<int>{0});
}

TEST_F(ContainerdDaemonTest, SighupAndConsoleMessagesDontStopIt) {
  Config({{"containers", nlohmann::json::array()}});
  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s));
  ASSERT_TRUE(ctrd.Channel().Send(aember::utils::ipc::Console(false)));
  ::kill(ctrd.Pid(), SIGHUP);
  EXPECT_FALSE(ctrd.Wait(500ms)) << "containerd exited on SIGHUP";
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
}

TEST_F(ContainerdDaemonTest, ContainerThatCannotStartIsRetriedAndReported) {
  // Installed (its directory), but its image is missing: the start fails.
  fs::create_directories(dir_ / "lib" / "broken");
  std::ofstream{dir_ / "lib" / "broken" / "config.json"} << nlohmann::json{
      {"name", "broken"},
      {"command", {"/bin/true"}},
      {"restart_policy", "always"},
      {"max_restart_attempts", 2},
      {"restart_delay", 1}}.dump();
  Config({{"containers", nlohmann::json::array({{{"name", "broken"}}})}});
  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  // The failed start shows up as the container's state, then the retries.
  const auto event = ctrd.Expect("event", 10s);
  ASSERT_TRUE(event) << Log();
  EXPECT_EQ((*event)["data"].value("name", ""), "broken");
  std::this_thread::sleep_for(2500ms);
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
  EXPECT_NE(Log().find("containerd ready (1 container(s))"), std::string::npos);
  EXPECT_NE(Log().find("restart 2/2"), std::string::npos) << Log();
}

TEST_F(ContainerdDaemonTest, NotInstalledOrSkippedIsReportedTheRestGoesOn) {
  Config({{"containers",
           nlohmann::json::array({{{"name", "missing"}},
                                  {{"name", "x"}, {"dependencies", {"y"}}},
                                  {{"name", "y"}, {"dependencies", {"x"}}}})}});
  auto ctrd = Run();
  // Reported before ready (nothing of it can start).
  const auto event = ctrd.Expect("event", 10s);
  ASSERT_TRUE(event) << Log();
  EXPECT_EQ((*event)["data"].value("name", ""), "missing");
  EXPECT_EQ((*event)["data"].value("state", ""), "failed");
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
  EXPECT_NE(Log().find("container 'x' not started"), std::string::npos)
      << Log();
}

namespace pkg = aember::utils::package;

void WritePackage(const fs::path& path, const std::string& name,
                  const std::optional<pkg::KeyPair>& key) {
  fs::create_directories(path.parent_path());
  const auto bytes = pkg::Seal(
      *pkg::Compress(pkg::MakeTar(
          nlohmann::json{{"name", name}, {"command", {"/app"}}}.dump(),
          "lxc.uts.name = " + name + "\n",
          "IMAGE")),
      key);
  std::ofstream{path, std::ios::binary}.write(
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
}

TEST_F(ContainerdDaemonTest, PackagesAreInstalledFromTheInbox) {
  std::array<std::uint8_t, 32> seed{};
  seed.fill(3);
  const auto key = pkg::KeyPairFromSeed(seed);
  fs::create_directories(dir_ / "keys");
  std::ofstream{dir_ / "keys" / "board.pub"}
      << pkg::PublicKeyText(key.public_key);
  const auto inbox = dir_ / "lib" / "packages";
  WritePackage(inbox / "good.aember", "good", key);
  WritePackage(inbox / "unsigned.aember", "unsigned", std::nullopt);
  WritePackage(inbox / "wrong.aember", "other", key);  // config names "other"
  Config({{"containers", nlohmann::json::array()}});   // signed only

  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
  EXPECT_TRUE(fs::exists(dir_ / "lib" / "good" / "image.squashfs")) << Log();
  EXPECT_FALSE(fs::exists(inbox / "good.aember"));
  EXPECT_NE(Log().find("installed good (signed by board"), std::string::npos)
      << Log();
  // Refused, kept aside so they aren't tried on every start.
  EXPECT_FALSE(fs::exists(dir_ / "lib" / "unsigned"));
  EXPECT_TRUE(fs::exists(inbox / "unsigned.aember.rejected"));
  EXPECT_TRUE(fs::exists(inbox / "wrong.aember.rejected"));
}

TEST_F(ContainerdDaemonTest, UnsignedPackagesOnlyWhenTheBoardAllowsThem) {
  WritePackage(dir_ / "lib" / "packages" / "dev.aember", "dev", std::nullopt);
  Config({{"allow_unsigned", true}, {"containers", nlohmann::json::array()}});
  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
  EXPECT_TRUE(fs::exists(dir_ / "lib" / "dev" / "package.json")) << Log();
  EXPECT_NE(Log().find("installed dev (unsigned"), std::string::npos) << Log();
}

TEST_F(ContainerdDaemonTest, BridgesAreCreatedAndAddressed) {
  if (::geteuid() != 0)
    GTEST_SKIP() << "needs root (private network namespace)";
  Config({{"bridges",
           nlohmann::json::array(
               {{{"name", "aemtest0"}, {"address", "10.123.45.1/24"}}})},
          {"containers", nlohmann::json::array()}});
  auto ctrd = Run();
  ASSERT_TRUE(ctrd.Expect("ready", 10s)) << Log();
  // The daemon's network namespace, seen through its /proc entry.
  const auto proc = fs::path{"/proc"} / std::to_string(ctrd.Pid()) / "net";
  EXPECT_NE(ReadFile(proc / "dev").find("aemtest0"), std::string::npos);
  EXPECT_NE(ReadFile(proc / "fib_trie").find("10.123.45.1"), std::string::npos);
  EXPECT_EQ(ctrd.Stop(SIGTERM, 15s), std::optional<int>{0});
}

TEST(EnsureBridgesTest, UnreadableConfigCreatesNothing) {
  EXPECT_EQ(aember::containerd::EnsureBridges("/nonexistent/containers.json"),
            0);
}

}  // namespace
