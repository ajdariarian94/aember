/**
 * @file control.cpp
 * @brief containerd's control socket: requests against a fake target, and
 *        one round trip over a real socket.
 */
#include <aember-services/containerd/control.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using aember::containerd::ControlTarget;
using aember::containerd::HandleControlRequest;
using nlohmann::json;

struct FakeTarget final : ControlTarget {
  std::map<std::string, std::string> states{{"web", "running"},
                                            {"db", "stopped"}};
  std::vector<std::string> calls;
  bool fail = false;
  [[nodiscard]] std::vector<Entry> List() const override {
    std::vector<Entry> list;
    for (const auto& [name, state] : states) list.push_back({name, state});
    return list;
  }
  [[nodiscard]] bool Has(std::string_view name) const override {
    return states.contains(std::string{name});
  }
  bool Stop(std::string_view name) override {
    calls.push_back("stop " + std::string{name});
    states[std::string{name}] = "stopped";
    return !fail;
  }
  bool Start(std::string_view name) override {
    calls.push_back("start " + std::string{name});
    states[std::string{name}] = "running";
    return !fail;
  }
  std::expected<void, std::string> Install(std::string_view name) override {
    calls.push_back("install " + std::string{name});
    if (fail) return std::unexpected("bad signature");
    return {};
  }
  std::expected<void, std::string> Rollback(std::string_view name) override {
    calls.push_back("rollback " + std::string{name});
    if (fail) return std::unexpected("no previous version");
    return {};
  }
};

TEST(ControlTest, InstallAndRollbackSayWhyTheyFailed) {
  FakeTarget t;
  EXPECT_TRUE(
      HandleControlRequest({{"op", "install"}, {"name", "web"}}, t)["ok"]);
  EXPECT_TRUE(
      HandleControlRequest({{"op", "rollback"}, {"name", "web"}}, t)["ok"]);
  EXPECT_EQ(t.calls, (std::vector<std::string>{"install web", "rollback web"}));
  t.fail = true;
  const auto a = HandleControlRequest({{"op", "install"}, {"name", "web"}}, t);
  EXPECT_FALSE(a["ok"].get<bool>());
  EXPECT_NE(a["error"].get<std::string>().find("bad signature"),
            std::string::npos);
  EXPECT_FALSE(
      HandleControlRequest({{"op", "install"}, {"name", "nope"}}, t)["ok"]);
  EXPECT_FALSE(HandleControlRequest({{"op", "install"}}, t)["ok"]);
}

TEST(ControlTest, ListsContainers) {
  FakeTarget t;
  const auto a = HandleControlRequest(json{{"op", "list"}}, t);
  ASSERT_TRUE(a["ok"].get<bool>());
  ASSERT_EQ(a["containers"].size(), 2u);
  EXPECT_EQ(a["containers"][0]["name"], "db");
  EXPECT_EQ(a["containers"][0]["state"], "stopped");
}

TEST(ControlTest, StopStartRestart) {
  FakeTarget t;
  EXPECT_TRUE(HandleControlRequest({{"op", "stop"}, {"name", "web"}}, t)["ok"]);
  EXPECT_TRUE(HandleControlRequest({{"op", "start"}, {"name", "db"}}, t)["ok"]);
  EXPECT_TRUE(
      HandleControlRequest({{"op", "restart"}, {"name", "web"}}, t)["ok"]);
  EXPECT_EQ(t.calls,
            (std::vector<std::string>{
                "stop web", "start db", "stop web", "start web"}));
}

TEST(ControlTest, BadRequestsAreAnsweredNotThrown) {
  FakeTarget t;
  for (const auto& req : {json::array(),
                          json{{"op", 3}},
                          json{{"op", "explode"}},
                          json{{"op", "stop"}},
                          json{{"op", "stop"}, {"name", "nope"}}}) {
    const auto a = HandleControlRequest(req, t);
    EXPECT_FALSE(a["ok"].get<bool>()) << req.dump();
    EXPECT_TRUE(a.contains("error")) << req.dump();
  }
  EXPECT_TRUE(t.calls.empty());
}

TEST(ControlTest, FailureIsReported) {
  FakeTarget t;
  t.fail = true;
  const auto a = HandleControlRequest({{"op", "restart"}, {"name", "web"}}, t);
  EXPECT_FALSE(a["ok"].get<bool>());
  EXPECT_EQ(t.calls, (std::vector<std::string>{"stop web"}));  // no start
}

TEST(ControlTest, RawRequestsGetJsonAnswers) {
  FakeTarget t;
  for (const std::string_view raw :
       {"", "{", "\xff\xfe", R"({"op": "list")", "null"}) {
    const auto a =
        json::parse(aember::containerd::AnswerControlRequest(raw, t));
    EXPECT_FALSE(a["ok"].get<bool>()) << raw;
  }
  const auto a = json::parse(
      aember::containerd::AnswerControlRequest(R"({"op":"list"})", t));
  EXPECT_TRUE(a["ok"].get<bool>());
}

TEST(ControlTest, RoundTripOverTheSocket) {
  char dir[] = "/tmp/aember-control-XXXXXX";
  ASSERT_NE(::mkdtemp(dir), nullptr);
  const std::string path = std::string{dir} + "/sub/control.sock";
  const int listen_fd = aember::containerd::OpenControlSocket(path);
  ASSERT_GE(listen_fd, 0);
  struct stat st {};
  ASSERT_EQ(::stat((std::string{dir} + "/sub").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700u);

  FakeTarget t;
  std::jthread server{[&] {
    for (int i = 0; i < 100; ++i) {
      if (aember::containerd::ServeControlConnection(listen_fd, t)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
  }};
  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  const std::string req = R"({"op":"stop","name":"web"})";
  ASSERT_EQ(::send(fd, req.data(), req.size(), 0),
            static_cast<ssize_t>(req.size()));
  char buf[512] = {};
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  ASSERT_GT(n, 0);
  EXPECT_TRUE(json::parse(std::string_view{buf, static_cast<size_t>(n)})["ok"]);
  ::close(fd);
  server.join();
  EXPECT_EQ(t.states["web"], "stopped");
  ::close(listen_fd);
  std::filesystem::remove_all(dir);
}

}  // namespace
