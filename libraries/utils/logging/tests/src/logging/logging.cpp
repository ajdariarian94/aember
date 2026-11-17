/**
 * @file logging.cpp
 * @author Arian Ajdari
 * @brief Logging tests: one spdlog logger for the whole process, component
 *        prefix, levels, file sink, console muting.
 * @version 0.2
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/logging/logging.h>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace logging = aember::utils::logging;
using logging::Logger;

std::string ReadFile(const fs::path& path) {
  logging::flush();
  std::ifstream file{path};
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

TEST(LoggingTest, OneSpdlogLoggerForAllComponents) {
  Logger a{"component-a"};
  Logger b{"component-b"};
  std::vector<Logger> many;
  for (int i = 0; i < 50; ++i) many.emplace_back("c" + std::to_string(i));

  // No per-component spdlog loggers are registered...
  EXPECT_EQ(spdlog::get("component-a"), nullptr);
  EXPECT_EQ(spdlog::get("c7"), nullptr);
  // ...everything goes through the one default logger.
  EXPECT_EQ(spdlog::default_logger_raw(), &logging::global_logger());
  EXPECT_EQ(&logging::global_logger(), &logging::global_logger());
}

TEST(LoggingTest, LoggerIsJustAName) {
  Logger log{"name-only"};
  EXPECT_EQ(log.name(), "name-only");
  Logger copy = log;
  EXPECT_EQ(copy.name(), "name-only");
}

TEST(LoggingTest, AllLevelsWork) {
  Logger log{"levels"};
  EXPECT_NO_THROW(log.trace("trace {}", 1));
  EXPECT_NO_THROW(log.debug("debug {}", 2));
  EXPECT_NO_THROW(log.info("info {}", 3));
  EXPECT_NO_THROW(log.warn("warn {}", 4));
  EXPECT_NO_THROW(log.error("error {}", 5));
  EXPECT_NO_THROW(log.critical("critical {}", 6));
}

TEST(LoggingTest, ConcurrentLoggingFromManyThreads) {
  std::vector<std::jthread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([t] {
      Logger log{"thread-" + std::to_string(t)};
      for (int i = 0; i < 200; ++i) log.debug("line {}", i);
    });
  }
}

// File logging is process-wide and can be enabled once, so the file
// behaviour is one ordered test.
TEST(LoggingTest, FileSinkPrefixLevelsAndConsoleMute) {
  const auto dir = fs::temp_directory_path() /
                   ("aember-logging-test-" + std::to_string(::getpid()));
  fs::remove_all(dir);

  // A file that can't be created is reported, not thrown, and file logging
  // can still be enabled afterwards.
  EXPECT_NO_THROW(logging::enable_file_logging("/proc/aember/cannot/log"));

  const auto path = dir / "sub" / "test.log";
  logging::enable_file_logging(path);
  ASSERT_TRUE(fs::exists(path));  // parent dirs created

  // A second call is ignored.
  EXPECT_NO_THROW(logging::enable_file_logging(dir / "other.log"));
  EXPECT_FALSE(fs::exists(dir / "other.log"));

  Logger log{"file-test"};
  logging::logging_level_info();
  log.info("visible {}", 42);
  log.debug("hidden-at-info");
  logging::logging_level_debug();
  log.debug("visible-at-debug");
  logging::logging_level_info();

  // Muting the console keeps the file.
  logging::set_console_logging(false);
  log.warn("while-muted");
  logging::set_console_logging(true);

  const auto text = ReadFile(path);
  EXPECT_NE(text.find("[file-test] visible 42"), std::string::npos) << text;
  EXPECT_NE(text.find("[info]"), std::string::npos);
  EXPECT_EQ(text.find("hidden-at-info"), std::string::npos);
  EXPECT_NE(text.find("[file-test] visible-at-debug"), std::string::npos);
  EXPECT_NE(text.find("[file-test] while-muted"), std::string::npos);
  EXPECT_NE(text.find("[warning]"), std::string::npos);

  fs::remove_all(dir);
}

}  // namespace
