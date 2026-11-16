/**
 * @file config-manager.cpp
 * @author Arian Ajdari
 * @brief Tests for ConfigManager and IConfigFileParser::LoadJsonFromFile:
 *        a broken config file must give an error, never an exception (an
 *        exception in PID 1 is a kernel panic).
 * @version 0.2
 * @date 2026-11-16
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/config/config-manager.h>
#include <aember-libs/utils/config/iconfig-file-parser.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;
using aember::utils::config::ConfigError;
using aember::utils::config::ConfigManager;
using aember::utils::config::IConfigFileParser;

/// Minimal parser: loads the JSON and requires a top-level object.
class ObjectParser : public IConfigFileParser {
 public:
  bool ParseFile(const std::string& path, ConfigError& error) override {
    if (!LoadJsonFromFile(path, json_, error)) { return false; }
    if (!json_.is_object()) {
      error = ConfigError("root must be an object", path);
      return false;
    }
    return true;
  }
  nlohmann::json json_;
};

class ConfigManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-config-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::string Write(const std::string& name, const std::string& content) {
    const auto path = dir_ / name;
    std::ofstream{path} << content;
    return path.string();
  }

  fs::path dir_;
  ObjectParser parser_;
  ConfigManager manager_;
};

TEST_F(ConfigManagerTest, LoadsValidFile) {
  EXPECT_TRUE(manager_.Load(parser_, Write("ok.json", R"({"a": 1})")));
  EXPECT_FALSE(manager_.GetLastError().has_value());
  EXPECT_EQ(parser_.json_["a"], 1);
}

TEST_F(ConfigManagerTest, MissingFileIsAnError) {
  EXPECT_NO_THROW(EXPECT_FALSE(manager_.Load(parser_, "/nonexistent.json")));
  ASSERT_TRUE(manager_.GetLastError().has_value());
  EXPECT_NE(manager_.GetLastError()->message.find("open"), std::string::npos);
}

TEST_F(ConfigManagerTest, MalformedJsonIsAnErrorNotAnException) {
  for (const auto* content : {"", "{", "{\"a\": }", "not json", "\xff\xfe"}) {
    ConfigManager manager;
    bool ok = true;
    EXPECT_NO_THROW(ok = manager.Load(parser_, Write("bad.json", content)))
        << "content: " << content;
    EXPECT_FALSE(ok) << "content: " << content;
    EXPECT_TRUE(manager.GetLastError().has_value());
  }
}

TEST_F(ConfigManagerTest, WrongRootTypeIsAnError) {
  EXPECT_FALSE(manager_.Load(parser_, Write("arr.json", "[1, 2]")));
  ASSERT_TRUE(manager_.GetLastError().has_value());
}

}  // namespace
