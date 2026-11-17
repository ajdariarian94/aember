/**
 * @file module-parser.cpp
 * @author Arian Ajdari
 * @brief ModulesConfigParser tests (modules.json): string and object
 *        entries, rejections.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/module/module-parser.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::utils::config::ConfigError;
using aember::utils::module::ModulesConfigParser;

class ModuleParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-module-test-" + std::to_string(::getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  bool Parse(const std::string& json) {
    const auto path = dir_ / "modules.json";
    std::ofstream{path} << json;
    error_ = {};
    bool ok = false;
    EXPECT_NO_THROW(ok = parser_.ParseFile(path.string(), error_));
    return ok;
  }

  std::vector<std::string> Names() const {
    std::vector<std::string> names;
    for (const auto& m : parser_.GetModules()) names.push_back(m.name);
    return names;
  }

  fs::path dir_;
  ModulesConfigParser parser_;
  ConfigError error_;
};

TEST_F(ModuleParserTest, StringEntries) {
  ASSERT_TRUE(Parse(R"({"modules": ["llc", "stp", "virtio-gpu"]})"));
  EXPECT_EQ(Names(), (std::vector<std::string>{"llc", "stp", "virtio-gpu"}));
}

TEST_F(ModuleParserTest, ObjectAndMixedEntries) {
  ASSERT_TRUE(Parse(R"({"modules": [{"name": "bridge"}, "veth"]})"));
  EXPECT_EQ(Names(), (std::vector<std::string>{"bridge", "veth"}));
}

TEST_F(ModuleParserTest, EmptyListIsValid) {
  EXPECT_TRUE(Parse(R"({"modules": []})"));
  EXPECT_TRUE(Names().empty());
}

TEST_F(ModuleParserTest, RejectsBadFiles) {
  EXPECT_FALSE(Parse(""));
  EXPECT_FALSE(Parse("{"));
  EXPECT_FALSE(Parse(R"(["llc"])"));
  EXPECT_FALSE(Parse(R"({"mods": ["llc"]})"));
  EXPECT_FALSE(Parse(R"({"modules": "llc"})"));
  ConfigError error;
  EXPECT_FALSE(parser_.ParseFile("/nonexistent/modules.json", error));
}

TEST_F(ModuleParserTest, RejectsBadEntries) {
  for (const auto* modules : {R"([""])",
                              R"([1])",
                              R"([null])",
                              R"([["llc"]])",
                              R"([{}])",
                              R"([{"name": 1}])",
                              R"([{"name": ""}])"}) {
    EXPECT_FALSE(Parse(std::string{R"({"modules": )"} + modules + "}"))
        << modules;
    EXPECT_FALSE(error_.message.empty()) << modules;
  }
}

}  // namespace
