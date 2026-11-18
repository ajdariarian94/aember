/**
 * @file aember-monitor.cpp
 * @author Arian Ajdari
 * @brief AemberMonitor tests: the /proc snapshot and the in-place JSON file
 *        (its inode is bind-mounted into the monitor container).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/aember-monitor.h>
#include <aember-libs/utils/logging/logging.h>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::device_health::AemberMonitor;
using aember::device_health::AemberMonitorClient;

std::string Read(const fs::path& p) {
  std::ifstream f{p};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

ino_t Inode(const fs::path& p) {
  struct stat st {};
  ::stat(p.c_str(), &st);
  return st.st_ino;
}

TEST(AemberMonitorTest, SnapshotHasPid1Fields) {
  AemberMonitor monitor;
  const auto snap = monitor.Snapshot();
  ASSERT_TRUE(snap.contains("pid1"));
  const auto& p = snap["pid1"];
  for (const auto* key : {"pid",
                          "vm_rss_kb",
                          "vm_size_kb",
                          "threads",
                          "children",
                          "uptime_seconds"}) {
    EXPECT_TRUE(p.contains(key)) << key;
  }
  EXPECT_EQ(p["pid"], 1);
  EXPECT_GE(p["uptime_seconds"].get<long>(), 0);
}

TEST(AemberMonitorTest, LogDoesNotThrowOnPartialPayloads) {
  AemberMonitor monitor;
  EXPECT_NO_THROW(monitor.Log(monitor.Snapshot()));
  EXPECT_NO_THROW(monitor.Log(nlohmann::json::object()));
  EXPECT_NO_THROW(monitor.Log(nlohmann::json{{"pid1", "garbage"}}));
}

// The network line says so when no service reports the network.
TEST(AemberMonitorTest, LogSaysWhenNoNetworkIsReported) {
  auto stream = std::make_shared<std::ostringstream>();
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream);
  auto& logger = aember::utils::logging::global_logger();
  logger.sinks().push_back(sink);
  AemberMonitor monitor;
  monitor.Log(nlohmann::json::object());
  monitor.Log(nlohmann::json{
      {"network",
       {{"online", true}, {"interface", "eth0"}, {"ip", "10.0.2.15"}}}});
  logger.flush();
  std::erase(logger.sinks(), sink);
  const auto text = stream->str();
  EXPECT_NE(text.find("Network      │ not reported"), std::string::npos)
      << text;
  EXPECT_NE(text.find("ONLINE  │ iface=eth0"), std::string::npos) << text;
}

TEST(AemberMonitorTest, LogToFileRewritesInPlace) {
  const auto path = fs::temp_directory_path() /
                    ("aember-monitor-" + std::to_string(::getpid()) + ".json");
  { std::ofstream{path} << "{}"; }
  const auto inode = Inode(path);

  AemberMonitor monitor;
  monitor.LogToFile(nlohmann::json{{"long", std::string(200, 'x')}},
                    path.string());
  EXPECT_EQ(nlohmann::json::parse(Read(path))["long"].get<std::string>().size(),
            200u);

  // Shorter content: the old tail must be truncated, the inode kept.
  monitor.LogToFile(nlohmann::json{{"a", 1}}, path.string());
  EXPECT_EQ(Read(path), R"({"a":1})");
  EXPECT_EQ(Inode(path), inode);
  fs::remove(path);
}

TEST(AemberMonitorTest, LogToFileCreatesMissingFileAndSurvivesBadPath) {
  const auto path = fs::temp_directory_path() /
                    ("aember-monitor-new-" + std::to_string(::getpid()));
  fs::remove(path);
  AemberMonitor monitor;
  monitor.LogToFile(nlohmann::json{{"b", 2}}, path.string());
  EXPECT_EQ(Read(path), R"({"b":2})");
  fs::remove(path);
  EXPECT_NO_THROW(
      monitor.LogToFile(nlohmann::json{{"c", 3}}, "/nonexistent/dir/x.json"));
}

TEST(AemberMonitorClientTest, SnapshotDescribesThisProcess) {
  AemberMonitorClient client{"unit-test"};
  const auto snap = client.Snapshot();
  EXPECT_EQ(snap["service"], "unit-test");
  EXPECT_EQ(snap["pid"].get<int>(), ::getpid());
  EXPECT_EQ(snap["ppid"].get<int>(), ::getppid());
  EXPECT_GT(snap["vm_rss_kb"].get<long>(), 0);
  EXPECT_NO_THROW(client.Report());
}

}  // namespace
