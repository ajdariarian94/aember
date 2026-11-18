/**
 * @file watchdog.cpp
 * @author Arian Ajdari
 * @brief Tests for the watchdog: a regular file stands in for
 *        /dev/watchdog (keepalives are writes, so they show up in it; the
 *        ioctls fail there and the requested timeout is kept).
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/watchdog/watchdog.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

namespace wd = aember::utils::watchdog;
using namespace std::chrono_literals;

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& name)
      : path{(std::filesystem::temp_directory_path() /
              (name + "-" + std::to_string(::getpid())))
                 .string()} {
    std::ofstream{path};  // create empty
  }
  ~TempFile() { std::filesystem::remove(path); }
  [[nodiscard]] std::string Content() const {
    std::ifstream f{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{f}, {}};
  }
};

std::string Cmdline(const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("aember-wd-cmdline-" + std::to_string(::getpid()));
  std::ofstream{path} << content;
  return path.string();
}

std::unique_ptr<wd::Watchdog> OpenFile(const TempFile& f) {
  return wd::Watchdog::Open({.device = f.path, .timeout = 1s});
}

TEST(WatchdogTest, OptionsFromCmdline) {
  auto o = wd::OptionsFromCmdline(Cmdline("console=ttyS0 root=/dev/vda rw"));
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->timeout, 15s);
  EXPECT_EQ(o->device, "/dev/watchdog");

  o = wd::OptionsFromCmdline(Cmdline("quiet aember.watchdog=30"));
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->timeout, 30s);

  for (const char* off : {"aember.watchdog=0",
                          "aember.watchdog=off",
                          "aember.watchdog=no",
                          "aember.watchdog=false"}) {
    EXPECT_FALSE(wd::OptionsFromCmdline(Cmdline(off)).has_value()) << off;
  }
  for (const char* bad : {"aember.watchdog=abc",
                          "aember.watchdog=-5",
                          "aember.watchdog=10s",
                          "aember.watchdog="}) {
    o = wd::OptionsFromCmdline(Cmdline(bad));
    ASSERT_TRUE(o.has_value()) << bad;
    EXPECT_EQ(o->timeout, 15s) << bad;
  }
  EXPECT_TRUE(wd::OptionsFromCmdline("/nonexistent/cmdline").has_value());
}

TEST(WatchdogTest, NoDeviceIsNull) {
  EXPECT_EQ(wd::Watchdog::Open({.device = "/nonexistent/watchdog"}), nullptr);
}

TEST(WatchdogTest, FeedsWhileAlive) {
  TempFile f{"aember-wd-alive"};
  auto dog = OpenFile(f);
  ASSERT_NE(dog, nullptr);
  EXPECT_EQ(dog->Timeout(), 1s);  // ioctl refused: the requested one
  dog->Start([] { return true; });
  std::this_thread::sleep_for(1200ms);  // interval = timeout / 3
  EXPECT_GE(dog->Feeds(), 3u);
  EXPECT_EQ(f.Content().size(), dog->Feeds());
}

// A hung PID 1 (alive check false) must not be covered up by the thread.
TEST(WatchdogTest, StopsFeedingWhenNotAlive) {
  TempFile f{"aember-wd-hung"};
  auto dog = OpenFile(f);
  ASSERT_NE(dog, nullptr);
  std::atomic<bool> alive{true};
  dog->Start([&] { return alive.load(); });
  std::this_thread::sleep_for(500ms);
  alive = false;
  std::this_thread::sleep_for(400ms);  // let a pending feed through
  const auto stalled = dog->Feeds();
  std::this_thread::sleep_for(1000ms);
  EXPECT_EQ(dog->Feeds(), stalled);
  alive = true;  // recovers
  std::this_thread::sleep_for(600ms);
  EXPECT_GT(dog->Feeds(), stalled);
}

TEST(WatchdogTest, ThrowingAliveCheckCountsAsHung) {
  TempFile f{"aember-wd-throw"};
  auto dog = OpenFile(f);
  ASSERT_NE(dog, nullptr);
  dog->Start([]() -> bool { throw std::runtime_error("bug"); });
  const auto first = dog->Feeds();  // Start feeds once
  std::this_thread::sleep_for(900ms);
  EXPECT_EQ(dog->Feeds(), first);
}

// Shutdown: services stop while the main loop no longer runs.
TEST(WatchdogTest, UnconditionalFeedsEvenWhenNotAlive) {
  TempFile f{"aember-wd-uncond"};
  auto dog = OpenFile(f);
  ASSERT_NE(dog, nullptr);
  dog->Start([] { return false; });
  dog->SetUnconditional(true);
  std::this_thread::sleep_for(1000ms);
  EXPECT_GE(dog->Feeds(), 3u);
}

TEST(WatchdogTest, DisarmWritesMagicCloseLast) {
  TempFile f{"aember-wd-disarm"};
  auto dog = OpenFile(f);
  ASSERT_NE(dog, nullptr);
  dog->Start([] { return true; });
  std::this_thread::sleep_for(500ms);
  EXPECT_TRUE(dog->Disarm());
  const auto content = f.Content();
  ASSERT_FALSE(content.empty());
  EXPECT_EQ(content.back(), 'V');
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(f.Content(), content);  // nothing after the magic close
  EXPECT_FALSE(dog->Feed());
}

// Destroying it without Disarm() leaves the watchdog armed: no 'V'.
TEST(WatchdogTest, DestructorLeavesItArmed) {
  TempFile f{"aember-wd-armed"};
  {
    auto dog = OpenFile(f);
    ASSERT_NE(dog, nullptr);
    dog->Start([] { return true; });
    std::this_thread::sleep_for(400ms);
  }
  const auto content = f.Content();
  EXPECT_FALSE(content.empty());
  EXPECT_EQ(content.find('V'), std::string::npos);
}

}  // namespace
