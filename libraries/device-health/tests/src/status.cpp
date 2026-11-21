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

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
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

}  // namespace
