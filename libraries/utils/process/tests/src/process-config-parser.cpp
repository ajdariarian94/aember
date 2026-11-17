/**
 * @file process-config-parser.cpp
 * @author Arian Ajdari
 * @brief ProcessConfigParser tests (services.json): every field, defaults
 *        and every rejection path. Bad input must give an error, never an
 *        exception (PID 1 parses this file).
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/process/process-config-parser.h>
#include <aember-libs/utils/process/process-state.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::utils::config::ConfigError;
using aember::utils::process::ProcessConfigParser;
using aember::utils::process::ProcessState;
using aember::utils::process::ToString;

class ProcessConfigParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-process-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  /// Parse @p json; returns success, error in error_.
  bool Parse(const std::string& json) {
    const auto path = dir_ / "services.json";
    std::ofstream{path} << json;
    error_ = {};
    bool ok = false;
    EXPECT_NO_THROW(ok = parser_.ParseFile(path.string(), error_));
    return ok;
  }

  /// One process entry inside a valid file.
  bool ParseOne(const std::string& entry) {
    return Parse(R"({"processes": [)" + entry + "]}");
  }

  fs::path dir_;
  ProcessConfigParser parser_;
  ConfigError error_;
};

TEST_F(ProcessConfigParserTest, FullEntry) {
  ASSERT_TRUE(ParseOne(R"({
    "name": "demo", "executable": "/usr/bin/demo",
    "description": "a demo", "args": ["-v", "--port", "80"],
    "working_directory": "/tmp", "environment": {"A": "1", "B": "two"},
    "dependencies": ["net", "db"], "restart_on_failure": false,
    "max_restarts": 3, "restart_delay_ms": 6000, "stop_timeout_ms": 250
  })"))
      << error_.message;

  const auto& procs = parser_.GetProcesses();
  ASSERT_EQ(procs.size(), 1u);
  const auto& p = procs[0];
  EXPECT_EQ(p.name, "demo");
  EXPECT_EQ(p.executable, "/usr/bin/demo");
  EXPECT_EQ(p.description, "a demo");
  EXPECT_EQ(p.args, (std::vector<std::string>{"-v", "--port", "80"}));
  EXPECT_EQ(p.working_directory, "/tmp");
  EXPECT_EQ(p.environment.at("A"), "1");
  EXPECT_EQ(p.environment.at("B"), "two");
  EXPECT_EQ(p.dependencies, (std::vector<std::string>{"net", "db"}));
  EXPECT_FALSE(p.restart_on_failure);
  EXPECT_EQ(p.max_restarts, 3u);
  EXPECT_EQ(p.restart_delay, std::chrono::milliseconds{6000});
  EXPECT_EQ(p.stop_timeout, std::chrono::milliseconds{250});
}

TEST_F(ProcessConfigParserTest, MinimalEntryGetsDefaults) {
  ASSERT_TRUE(ParseOne(R"({"name": "a", "executable": "/bin/a"})"));
  const auto& p = parser_.GetProcesses().at(0);
  EXPECT_TRUE(p.args.empty());
  EXPECT_TRUE(p.environment.empty());
  EXPECT_TRUE(p.dependencies.empty());
  EXPECT_TRUE(p.restart_on_failure);
  EXPECT_EQ(p.max_restarts, 0u);
  EXPECT_EQ(p.restart_delay, std::chrono::seconds{1});
  EXPECT_EQ(p.stop_timeout, std::chrono::seconds{5});
}

TEST_F(ProcessConfigParserTest, SeveralEntriesKeepOrder) {
  ASSERT_TRUE(Parse(R"({"processes": [
    {"name": "one", "executable": "/1"},
    {"name": "two", "executable": "/2"},
    {"name": "three", "executable": "/3"}]})"));
  const auto& procs = parser_.GetProcesses();
  ASSERT_EQ(procs.size(), 3u);
  EXPECT_EQ(procs[0].name, "one");
  EXPECT_EQ(procs[2].name, "three");
}

TEST_F(ProcessConfigParserTest, EmptyListIsValid) {
  EXPECT_TRUE(Parse(R"({"processes": []})"));
  EXPECT_TRUE(parser_.GetProcesses().empty());
}

TEST_F(ProcessConfigParserTest, ReparseReplacesPreviousResult) {
  ASSERT_TRUE(ParseOne(R"({"name": "old", "executable": "/o"})"));
  ASSERT_TRUE(ParseOne(R"({"name": "new", "executable": "/n"})"));
  ASSERT_EQ(parser_.GetProcesses().size(), 1u);
  EXPECT_EQ(parser_.GetProcesses()[0].name, "new");
}

TEST_F(ProcessConfigParserTest, RejectsBadFiles) {
  EXPECT_FALSE(Parse(""));
  EXPECT_FALSE(Parse("{"));
  EXPECT_FALSE(Parse("[]"));
  EXPECT_FALSE(Parse(R"({"services": []})"));
  EXPECT_FALSE(Parse(R"({"processes": {}})"));
  EXPECT_FALSE(error_.message.empty());

  ConfigError error;
  EXPECT_FALSE(parser_.ParseFile("/nonexistent/services.json", error));
  EXPECT_FALSE(error.message.empty());
}

TEST_F(ProcessConfigParserTest, RejectsMissingRequiredFields) {
  EXPECT_FALSE(ParseOne(R"({"executable": "/bin/a"})"));
  EXPECT_FALSE(ParseOne(R"({"name": "a"})"));
  EXPECT_FALSE(ParseOne(R"({"name": 5, "executable": "/bin/a"})"));
  EXPECT_FALSE(ParseOne(R"({"name": "a", "executable": ["/bin/a"]})"));
  EXPECT_FALSE(ParseOne(R"("just a string")"));
}

TEST_F(ProcessConfigParserTest, RejectsWrongFieldTypes) {
  const std::string base = R"("name": "a", "executable": "/bin/a", )";
  for (const auto* field : {
           R"("description": 1)",
           R"("args": "-v")",
           R"("args": ["-v", 2])",
           R"("working_directory": false)",
           R"("environment": ["A=1"])",
           R"("environment": {"A": 1})",
           R"("dependencies": "net")",
           R"("dependencies": [1])",
           R"("restart_on_failure": "yes")",
           R"("max_restarts": -1)",
           R"("max_restarts": "3")",
           R"("restart_delay_ms": -5)",
           R"("stop_timeout_ms": 1.5)",
       }) {
    EXPECT_FALSE(ParseOne("{" + base + field + "}")) << field;
  }
}

TEST_F(ProcessConfigParserTest, OneBadEntryFailsTheFile) {
  EXPECT_FALSE(Parse(R"({"processes": [
    {"name": "good", "executable": "/g"},
    {"name": "bad"}]})"));
}

TEST(ProcessStateTest, ToStringCoversEveryState) {
  for (const auto s : {ProcessState::Stopped,
                       ProcessState::Starting,
                       ProcessState::Running,
                       ProcessState::Stopping,
                       ProcessState::Restarting,
                       ProcessState::Failed}) {
    EXPECT_FALSE(ToString(s).empty());
    EXPECT_NE(ToString(s), "unknown");
  }
  EXPECT_NE(ToString(ProcessState::Running), ToString(ProcessState::Stopped));
}

}  // namespace
