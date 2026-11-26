/**
 * @file status.cpp
 * @author Arian Ajdari
 * @brief StatusReporter tests: the /proc snapshot and the in-place status
 *        file (/var/log/aember-status.json, read by the aember-stats
 *        container through /host-logs).
 * @version 0.2
 * @date 2026-11-20
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/status.h>

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::device_health::StatusReporter;

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

TEST(StatusReporterTest, SnapshotHasPid1Fields) {
  StatusReporter status;
  const auto snap = status.Snapshot();
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

TEST(StatusReporterTest, Pid1StatsAreReadFromProc) {
  // PID 1 is whatever runs as init here (in a PID namespace: this test).
  const auto s = StatusReporter::ReadPid1Stats();
  EXPECT_GT(s.vm_rss_kb, 0);  // the value after "VmRSS:", not 0
  EXPECT_GE(s.vm_size_kb, s.vm_rss_kb);
  EXPECT_GE(s.threads, 1);
  EXPECT_GE(s.children, 0);
}

TEST(StatusReporterTest, CountsChildrenBothWays) {
  std::vector<pid_t> kids;
  for (int i = 0; i < 2; ++i) {
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::pause();
      ::_exit(0);
    }
    kids.push_back(pid);
  }
  EXPECT_EQ(StatusReporter::CountChildren(::getpid()), 2);
  EXPECT_EQ(StatusReporter::CountChildrenByScan(::getpid()), 2);
  for (const pid_t pid : kids) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
  }
  EXPECT_EQ(StatusReporter::CountChildren(::getpid()), 0);
  EXPECT_EQ(StatusReporter::CountChildrenByScan(::getpid()), 0);
}

// The scan reads the ppid from each /proc/<n>/stat (parsed from the last
// ')': comm may hold spaces and parentheses); other entries are skipped.
TEST(StatusReporterTest, CountChildrenByScanParsesStatLines) {
  const auto root = fs::temp_directory_path() /
                    ("aember-status-proc-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto stat = [&](const std::string& dir, const std::string& line) {
    fs::create_directories(root / dir);
    std::ofstream{root / dir / "stat"} << line << "\n";
  };
  stat("10", "10 (a (b) c) S 7 1 1");   // a child of 7
  stat("11", "11 (x) R 7 1 1");         // another
  stat("12", "12 (x) S 8 1 1");         // someone else's
  stat("13", "13 (x)");                 // too short
  stat("1x", "14 (x) S 7 1 1");         // not a pid directory
  fs::create_directories(root / "15");  // no stat
  EXPECT_EQ(StatusReporter::CountChildrenByScan(7, root), 2);
  EXPECT_EQ(StatusReporter::CountChildren(7, root), 2);  // no children file
  fs::remove_all(root);
}

TEST(StatusReporterTest, DefaultFileIsAemberStatusJson) {
  EXPECT_EQ(aember::device_health::kStatusFile, "/var/log/aember-status.json");
}

TEST(StatusReporterTest, WriteFileRewritesInPlace) {
  const auto path = fs::temp_directory_path() /
                    ("aember-status-" + std::to_string(::getpid()) + ".json");
  { std::ofstream{path} << "{}"; }
  const auto inode = Inode(path);

  StatusReporter status;
  status.WriteFile(nlohmann::json{{"long", std::string(200, 'x')}},
                   path.string());
  EXPECT_EQ(nlohmann::json::parse(Read(path))["long"].get<std::string>().size(),
            200u);

  // Shorter content: the old tail must be truncated, the inode kept.
  status.WriteFile(nlohmann::json{{"a", 1}}, path.string());
  EXPECT_EQ(Read(path), R"({"a":1})");
  EXPECT_EQ(Inode(path), inode);
  fs::remove(path);
}

TEST(StatusReporterTest, WriteFileCreatesMissingFileAndSurvivesBadPath) {
  const auto path = fs::temp_directory_path() /
                    ("aember-status-new-" + std::to_string(::getpid()));
  fs::remove(path);
  StatusReporter status;
  status.WriteFile(nlohmann::json{{"b", 2}}, path.string());
  EXPECT_EQ(Read(path), R"({"b":2})");
  fs::remove(path);
  EXPECT_NO_THROW(
      status.WriteFile(nlohmann::json{{"c", 3}}, "/nonexistent/dir/x.json"));
}

TEST(StatusReporterTest, WriteFileReplacesBytesThatArentUtf8) {
  const auto path = fs::temp_directory_path() /
                    ("aember-status-utf8-" + std::to_string(::getpid()));
  { std::ofstream{path} << R"({"old":1})"; }
  StatusReporter status;
  status.WriteFile(nlohmann::json{{"name",
                                   std::string{"eth\xff"
                                               "0"}}},
                   path.string());
  const auto written = nlohmann::json::parse(Read(path));
  EXPECT_EQ(written["name"],
            "eth\xEF\xBF\xBD"
            "0");  // U+FFFD
  fs::remove(path);
}

TEST(StatusReporterTest, ParseUptimeSeconds) {
  using aember::device_health::ParseUptimeSeconds;
  EXPECT_EQ(ParseUptimeSeconds("12345.67 54321.00\n"), 12345);
  EXPECT_EQ(ParseUptimeSeconds("0.50 1.00"), 0);
  EXPECT_EQ(ParseUptimeSeconds(""), 0);
  EXPECT_EQ(ParseUptimeSeconds("abc"), 0);
  EXPECT_EQ(ParseUptimeSeconds("-5 1"), 0);
  EXPECT_EQ(ParseUptimeSeconds("inf 1"), 0);
  EXPECT_EQ(ParseUptimeSeconds("nan 1"), 0);
  EXPECT_EQ(ParseUptimeSeconds("1e300"), std::numeric_limits<long>::max());
}

}  // namespace
