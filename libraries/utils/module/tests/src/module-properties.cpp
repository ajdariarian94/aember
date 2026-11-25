/**
 * @file module-properties.cpp
 * @author Arian Ajdari
 * @brief Property tests for modules.json (ModulesConfigParser): names in
 *        either form come back in order; anything else is an error.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/module/module-parser.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <aember-test/property.h>

namespace {

using aember::test::Gen;
using aember::utils::config::ConfigError;
using aember::utils::module::ModulesConfigParser;

bool ParseText(const std::string& text, ModulesConfigParser& parser,
               ConfigError& error) {
  const std::string path =
      std::filesystem::temp_directory_path() /
      ("aember-modules-" + std::to_string(::getpid()) + ".json");
  std::ofstream{path} << text;
  const bool ok = parser.ParseFile(path, error);
  std::filesystem::remove(path);
  return ok;
}

TEST(ModulesConfigProperty, NamesInEitherFormComeBackInOrder) {
  aember::test::ForAll(300, [](Gen& g) {
    std::vector<std::string> names;
    auto list = nlohmann::json::array();
    for (int i = g.Size(8); i > 0; --i) {
      names.push_back(g.Text("abcdefghijklmnopqrstuvwxyz0123456789_-", 1, 16));
      if (g.Chance(0.5)) {
        list.push_back(names.back());
      } else {
        list.push_back(
            {{"name", names.back()}, {"note", "extra keys are fine"}});
      }
    }
    ModulesConfigParser parser;
    ConfigError error;
    ASSERT_TRUE(
        ParseText(nlohmann::json{{"modules", list}}.dump(), parser, error))
        << error.message;
    std::vector<std::string> got;
    for (const auto& m : parser.GetModules()) got.push_back(m.name);
    EXPECT_EQ(got, names);
  });
}

TEST(ModulesConfigProperty, ABadEntryAnywhereFailsTheFile) {
  const std::vector<nlohmann::json> bad{nullptr,
                                        1,
                                        true,
                                        "",
                                        nlohmann::json::array({"kvm"}),
                                        nlohmann::json::object(),
                                        nlohmann::json::object({{"name", 3}}),
                                        nlohmann::json::object({{"name", ""}})};
  aember::test::ForAll(300, [&](Gen& g) {
    auto list = nlohmann::json::array();
    for (int i = g.Size(5); i > 0; --i) list.push_back("ok");
    const auto at = g.Int<std::size_t>(0, list.size());
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), g.Pick(bad));
    ModulesConfigParser parser;
    ConfigError error;
    EXPECT_FALSE(
        ParseText(nlohmann::json{{"modules", list}}.dump(), parser, error))
        << list.dump();
    EXPECT_FALSE(error.message.empty());
  });
}

}  // namespace
