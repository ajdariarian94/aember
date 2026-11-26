/**
 * @file ipc.cpp
 * @author Arian Ajdari
 * @brief Channel and Hub tests (PID 1 <-> daemon messages).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/ipc/channel.h>
#include <aember-libs/utils/ipc/hub.h>
#include <aember-libs/utils/logging/logging.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace ipc = aember::utils::ipc;
using namespace std::chrono_literals;
using nlohmann::json;

std::pair<ipc::Channel, ipc::Channel> MakePair() {
  auto pair = ipc::Channel::Pair();
  EXPECT_TRUE(pair.has_value());
  return std::move(*pair);
}

TEST(ChannelTest, MessagesArriveWhole) {
  auto [a, b] = MakePair();
  ASSERT_TRUE(a.Send(ipc::Ready()));
  ASSERT_TRUE(
      a.Send(ipc::Event("network", {{"online", true}, {"ip", "10.0.2.15"}})));

  const auto m1 = b.Receive(1s);
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(ipc::TypeOf(*m1), "ready");

  const auto m2 = b.Receive(1s);
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(ipc::TypeOf(*m2), "event");
  EXPECT_EQ((*m2)["name"], "network");
  EXPECT_EQ((*m2)["data"]["ip"], "10.0.2.15");
}

// Regression: Send threw nlohmann's type_error 316 on a string that isn't
// UTF-8 (found by fuzz/fuzz_ipc). Send is documented as returning false on
// failure; an exception out of a daemon's report thread is std::terminate.
TEST(ChannelTest, SendOfBytesThatArentUtf8NeverThrows) {
  auto [a, b] = MakePair();
  bool sent = false;
  EXPECT_NO_THROW(
      sent = a.Send(ipc::Event("network", {{"interface", "eth\x91"}})));
  ASSERT_TRUE(sent);
  const auto m = b.Receive(1s);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)["data"]["interface"], "eth\xEF\xBF\xBD");  // U+FFFD
}

TEST(ChannelTest, BothDirections) {
  auto [a, b] = MakePair();
  ASSERT_TRUE(b.Send({{"type", "hello"}}));
  const auto m = a.Receive(1s);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(ipc::TypeOf(*m), "hello");
}

TEST(ChannelTest, TimeoutWithoutMessage) {
  auto [a, b] = MakePair();
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(b.Receive(100ms).has_value());
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 90ms);
  EXPECT_FALSE(b.Closed());
}

TEST(ChannelTest, PeerExitIsClosed) {
  auto [a, b] = MakePair();
  { auto gone = std::move(a); }  // peer closes
  EXPECT_FALSE(b.Receive(1s).has_value());
  EXPECT_TRUE(b.Closed());
  EXPECT_FALSE(b.Receive(1s).has_value());  // stays closed
  EXPECT_FALSE(b.Send(ipc::Ready()));       // EPIPE, no SIGPIPE
}

TEST(ChannelTest, TooBigIsRefused) {
  auto [a, b] = MakePair();
  const json big = {{"type", "event"},
                    {"data", std::string(ipc::kMaxMessage, 'x')}};
  EXPECT_FALSE(a.Send(big));
}

TEST(ChannelTest, GarbageIsSkipped) {
  auto [a, b] = MakePair();
  const std::string garbage = "{not json";
  ASSERT_EQ(::send(a.Fd(), garbage.data(), garbage.size(), 0),
            static_cast<ssize_t>(garbage.size()));
  const std::string array = "[1,2]";  // valid JSON, not an object
  ::send(a.Fd(), array.data(), array.size(), 0);
  ASSERT_TRUE(a.Send(ipc::Ready()));
  EXPECT_FALSE(b.Receive(1s).has_value());
  EXPECT_FALSE(b.Receive(1s).has_value());
  const auto ok = b.Receive(1s);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ipc::TypeOf(*ok), "ready");
  EXPECT_FALSE(b.Closed());
}

TEST(ChannelTest, InvalidChannel) {
  ipc::Channel none;
  EXPECT_FALSE(none.Valid());
  EXPECT_FALSE(none.Send(ipc::Ready()));
  EXPECT_FALSE(none.Receive(10ms).has_value());
}

TEST(ChannelTest, TypeOf) {
  EXPECT_EQ(ipc::TypeOf(json{{"type", "ready"}}), "ready");
  EXPECT_EQ(ipc::TypeOf(json{{"type", 5}}), "");
  EXPECT_EQ(ipc::TypeOf(json::array()), "");
  EXPECT_EQ(ipc::TypeOf(json::object()), "");
}

// What PID 1 does for a daemon: the child gets its end on fd 3.
TEST(ChannelTest, ForkedChildInheritsFd3) {
  auto [parent, child_end] = MakePair();
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::dup2(child_end.Fd(), ipc::kInheritedFd);
    auto ch = ipc::Channel::Inherited();
    if (!ch.Valid()) ::_exit(2);
    if (!ch.Send(ipc::Event("hello", {{"pid", ::getpid()}}))) ::_exit(3);
    if (!ch.Send(ipc::Ready())) ::_exit(4);
    ::_exit(0);
  }
  child_end = ipc::Channel{};  // parent closes its copy of the child end

  const auto hello = parent.Receive(2s);
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ((*hello)["data"]["pid"], pid);
  const auto ready = parent.Receive(2s);
  ASSERT_TRUE(ready.has_value());
  EXPECT_EQ(ipc::TypeOf(*ready), "ready");

  int status = 0;
  ::waitpid(pid, &status, 0);
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_FALSE(parent.Receive(1s).has_value());
  EXPECT_TRUE(parent.Closed());  // child exited: EOF
}

TEST(ChannelTest, InheritedWithoutSocketIsInvalid) {
  // In this test process fd 3 is either closed or not a socket.
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::close(ipc::kInheritedFd);
    ::_exit(ipc::Channel::Inherited().Valid() ? 1 : 0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

// ---------------------------------------------------------------------------
// Hub
// ---------------------------------------------------------------------------

class HubTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hub_.Start(
        [this](const std::string& name, const json& m) {
          std::lock_guard lock{mutex_};
          messages_.emplace_back(name, ipc::TypeOf(m));
          cv_.notify_all();
        },
        [this](const std::string& name) {
          std::lock_guard lock{mutex_};
          closed_.push_back(name);
          cv_.notify_all();
        });
  }

  bool WaitFor(std::size_t messages, std::size_t closed = 0) {
    std::unique_lock lock{mutex_};
    return cv_.wait_for(lock, 3s, [&] {
      return messages_.size() >= messages && closed_.size() >= closed;
    });
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::pair<std::string, std::string>> messages_;
  std::vector<std::string> closed_;
  // Regression: last, so it is destroyed (its thread stopped) first. It was
  // first, and its thread could still push into closed_ after closed_ was
  // gone (valgrind: a "leak" from a vector written after destruction).
  ipc::Hub hub_;
};

TEST_F(HubTest, RoutesMessagesByChannelName) {
  auto [netd_pid1, netd] = MakePair();
  auto [ctrd_pid1, ctrd] = MakePair();
  hub_.Add("netd", std::move(netd_pid1));
  hub_.Add("containerd", std::move(ctrd_pid1));
  EXPECT_EQ(hub_.Size(), 2u);

  ASSERT_TRUE(netd.Send(ipc::Ready()));
  ASSERT_TRUE(ctrd.Send(ipc::Event("container", {})));
  ASSERT_TRUE(WaitFor(2));
  std::lock_guard lock{mutex_};
  EXPECT_NE(
      std::ranges::find(messages_,
                        std::pair<std::string, std::string>{"netd", "ready"}),
      messages_.end());
  EXPECT_NE(std::ranges::find(
                messages_,
                std::pair<std::string, std::string>{"containerd", "event"}),
            messages_.end());
}

TEST_F(HubTest, PeerExitIsReportedAndRemoved) {
  auto [mine, theirs] = MakePair();
  hub_.Add("netd", std::move(mine));
  { auto gone = std::move(theirs); }
  ASSERT_TRUE(WaitFor(0, 1));
  std::lock_guard lock{mutex_};
  EXPECT_EQ(closed_.front(), "netd");
  EXPECT_EQ(hub_.Size(), 0u);
}

// A restarted daemon brings a new channel under the same name.
TEST_F(HubTest, ReplacingAChannel) {
  auto [old_mine, old_theirs] = MakePair();
  auto [new_mine, new_theirs] = MakePair();
  hub_.Add("netd", std::move(old_mine));
  hub_.Add("netd", std::move(new_mine));
  EXPECT_EQ(hub_.Size(), 1u);
  ASSERT_TRUE(new_theirs.Send(ipc::Ready()));
  ASSERT_TRUE(WaitFor(1));
  EXPECT_FALSE(old_theirs.Send(ipc::Ready()));  // old one was closed
}

// Regression: a throwing handler (PID 1's first network event) ended the
// hub thread, so readiness and status were never seen again.
TEST(HubThrowTest, ThrowingHandlerDoesNotStopTheHub) {
  ipc::Hub hub;
  std::atomic<int> calls{0};
  hub.Start([&](const std::string&, const json&) {
    ++calls;
    throw std::runtime_error("handler bug");
  });
  auto [mine, theirs] = MakePair();
  hub.Add("netd", std::move(mine));
  ASSERT_TRUE(theirs.Send(ipc::Ready()));
  ASSERT_TRUE(theirs.Send(ipc::Ready()));
  for (int i = 0; i < 300 && calls < 2; ++i) std::this_thread::sleep_for(10ms);
  EXPECT_EQ(calls.load(), 2);  // the second message still arrived
}

TEST_F(HubTest, StopIsPrompt) {
  auto [mine, theirs] = MakePair();
  hub_.Add("x", std::move(mine));
  const auto t0 = std::chrono::steady_clock::now();
  hub_.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

// ---------------------------------------------------------------------------
// Console control (debug shell open -> daemons mute their console log)
// ---------------------------------------------------------------------------

std::optional<bool> ConsoleState(ipc::Channel& c) {
  const auto m = c.Receive(500ms);
  if (!m || ipc::TypeOf(*m) != "console") return std::nullopt;
  return m->at("enabled").get<bool>();
}

// Published state goes to every channel now and to channels added later
// (a daemon restarted while a shell is open starts muted).
TEST_F(HubTest, PublishReachesCurrentAndLaterChannels) {
  auto [netd_pid1, netd] = MakePair();
  hub_.Add("netd", std::move(netd_pid1));

  hub_.Publish("console", ipc::Console(false));
  EXPECT_EQ(ConsoleState(netd), std::optional<bool>{false});

  auto [ctrd_pid1, ctrd] = MakePair();
  hub_.Add("containerd", std::move(ctrd_pid1));
  EXPECT_EQ(ConsoleState(ctrd), std::optional<bool>{false});

  // A newer message with the same key replaces the old one.
  hub_.Publish("console", ipc::Console(true));
  EXPECT_EQ(ConsoleState(netd), std::optional<bool>{true});
  EXPECT_EQ(ConsoleState(ctrd), std::optional<bool>{true});
  auto [late_pid1, late] = MakePair();
  hub_.Add("late", std::move(late_pid1));
  EXPECT_EQ(ConsoleState(late), std::optional<bool>{true});
  EXPECT_FALSE(late.Receive(100ms).has_value());  // only the latest
}

// PID 1 must not hang on a daemon that never reads its channel.
TEST_F(HubTest, PublishNeverBlocks) {
  auto [mine, theirs] = MakePair();
  hub_.Add("stuck", std::move(mine));
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 10000; ++i) {
    hub_.Publish("console", ipc::Console(i % 2 == 0));
  }
  // A blocked Publish would hang for good once the socket buffer is full;
  // 10 s leaves room for valgrind (~3 s) and slow CI machines.
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 10s);
}

TEST(ControlTest, ApplyControlMutesTheConsoleLog) {
  ASSERT_TRUE(aember::utils::logging::console_logging_enabled());
  EXPECT_TRUE(ipc::ApplyControl(ipc::Console(false)));
  EXPECT_FALSE(aember::utils::logging::console_logging_enabled());
  EXPECT_TRUE(ipc::ApplyControl(ipc::Console(true)));
  EXPECT_TRUE(aember::utils::logging::console_logging_enabled());

  EXPECT_FALSE(ipc::ApplyControl(ipc::Ready()));
  EXPECT_FALSE(ipc::ApplyControl(json{{"type", "console"}}));
  EXPECT_FALSE(ipc::ApplyControl(json{{"type", "console"}, {"enabled", 1}}));
  EXPECT_TRUE(aember::utils::logging::console_logging_enabled());
}

// PID 1 -> containerd: every service's state (containers.json "services").
TEST(ControlTest, ServiceStatesRoundTrip) {
  const std::map<std::string, std::string> states{
      {"aember-netd", "running"}, {"aember-containerd", "starting"}};
  const auto message = ipc::Services(states);
  EXPECT_EQ(ipc::TypeOf(message), "services");
  EXPECT_EQ(ipc::ServiceStatesOf(message), states);
  EXPECT_FALSE(ipc::ApplyControl(message)) << "not a console message";

  EXPECT_FALSE(ipc::ServiceStatesOf(ipc::Ready()));
  EXPECT_FALSE(ipc::ServiceStatesOf(json{{"type", "services"}}));
  EXPECT_FALSE(
      ipc::ServiceStatesOf(json{{"type", "services"}, {"states", "x"}}));
  const auto some = ipc::ServiceStatesOf(
      json{{"type", "services"}, {"states", {{"a", "running"}, {"b", 1}}}});
  ASSERT_TRUE(some);
  EXPECT_EQ(*some, (std::map<std::string, std::string>{{"a", "running"}}));
}

TEST(ControlTest, ReceiveControlHandsOtherMessagesOn) {
  auto [mine, theirs] = MakePair();
  ASSERT_TRUE(mine.Send(ipc::Console(false)));
  ASSERT_TRUE(mine.Send(ipc::Services({{"aember-netd", "running"}})));
  ASSERT_TRUE(mine.Send(ipc::Console(true)));
  std::vector<std::string> other;
  ipc::ReceiveControl(theirs,
                      [&](const json& m) { other.push_back(ipc::TypeOf(m)); });
  EXPECT_EQ(other, std::vector<std::string>{"services"});
  EXPECT_TRUE(aember::utils::logging::console_logging_enabled());
}

}  // namespace
