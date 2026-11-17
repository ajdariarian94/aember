/**
 * @file module-loader.cpp
 * @author Arian Ajdari
 * @brief ModuleLoader tests without root: modules.json loading, queries and
 *        a failing load. Real loading is covered by the QEMU boots.
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/module-loader/module-loader.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using aember::module_loader::ModuleLoader;
using aember::utils::module::ModuleStatus;

fs::path WriteJson(const std::string& name, const std::string& content) {
  const auto path = fs::temp_directory_path() /
                    ("aember-ml-" + std::to_string(::getpid()) + "-" + name);
  std::ofstream{path} << content;
  return path;
}

TEST(ModuleLoaderTest, LoadModulesReadsTheList) {
  ModuleLoader loader;
  const auto modules = loader.LoadModules(
      WriteJson("ok.json", R"({"modules": ["llc", {"name": "virtio-gpu"}]})")
          .string());
  ASSERT_EQ(modules.size(), 2u);
  EXPECT_EQ(modules[0].name, "llc");
  EXPECT_EQ(modules[1].name, "virtio-gpu");
}

TEST(ModuleLoaderTest, LoadModulesFromBadFilesIsEmpty) {
  ModuleLoader loader;
  EXPECT_TRUE(loader.LoadModules("/nonexistent/modules.json").empty());
  EXPECT_TRUE(
      loader.LoadModules(WriteJson("bad.json", "{ nope").string()).empty());
  EXPECT_TRUE(
      loader.LoadModules(WriteJson("empty.json", R"({"modules": []})").string())
          .empty());
}

// Regression: IsLoaded used to match any word of /proc/modules, so the
// state column ("Live") counted as a loaded module.
TEST(ModuleLoaderTest, IsLoadedOnlyMatchesModuleNames) {
  ModuleLoader loader;
  EXPECT_FALSE(loader.IsLoaded("Live"));
  EXPECT_FALSE(loader.IsLoaded("0x0000000000000000"));
  EXPECT_FALSE(loader.IsLoaded("aember_no_such_module"));
}

TEST(ModuleLoaderTest, IsBuiltinOfUnknownModule) {
  ModuleLoader loader;
  EXPECT_FALSE(loader.IsBuiltin("aember_no_such_module"));
}

TEST(ModuleLoaderTest, EmptyListLoadsTrivially) {
  ModuleLoader loader;
  EXPECT_TRUE(loader.Load(std::vector<ModuleLoader::ModuleConfig>{}));
  EXPECT_TRUE(loader.GetResults().empty());
}

TEST(ModuleLoaderTest, UnknownModuleFailsWithAResult) {
  ModuleLoader loader;
  EXPECT_FALSE(loader.Load("aember_no_such_module").has_value());
  ASSERT_EQ(loader.GetResults().size(), 1u);
  EXPECT_EQ(loader.GetResults()[0].name, "aember_no_such_module");
  EXPECT_EQ(loader.GetResults()[0].status, ModuleStatus::Failed);
}

TEST(ModuleLoaderTest, ListWithAFailureReportsFalse) {
  ModuleLoader loader;
  std::vector<ModuleLoader::ModuleConfig> modules(1);
  modules[0].name = "aember_no_such_module";
  EXPECT_FALSE(loader.Load(modules));
}

}  // namespace
