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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// A module that is loaded counts (the first one in /proc/modules, if the
// kernel has any loaded).
TEST(ModuleLoaderTest, IsLoadedFindsALoadedModule) {
  std::ifstream proc{"/proc/modules"};
  std::string line;
  if (!std::getline(proc, line)) GTEST_SKIP() << "no loaded modules here";
  const auto name = line.substr(0, line.find(' '));
  ModuleLoader loader;
  EXPECT_TRUE(loader.IsLoaded(name)) << name;
}

TEST(ModuleLoaderTest, ListWithAFailureReportsFalse) {
  ModuleLoader loader;
  std::vector<ModuleLoader::ModuleConfig> modules(1);
  modules[0].name = "aember_no_such_module";
  EXPECT_FALSE(loader.Load(modules));
}

}  // namespace

// ---------------------------------------------------------------------------
// modules.dep + finit_module (replacing BusyBox modprobe)
// ---------------------------------------------------------------------------

namespace {

/// A fake /lib/modules/<release> with a modules.dep and modules.builtin.
class FakeModulesDir : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("aember-ml-dir-" + std::to_string(::getpid()));
    fs::create_directories(dir_ / "kernel/drivers/gpu");
    std::ofstream{dir_ / "modules.dep"}
        << "kernel/drivers/gpu/virtio-gpu.ko: "
           "kernel/drivers/gpu/drm_kms_helper.ko "
           "kernel/drivers/gpu/drm_shmem_helper.ko kernel/drivers/gpu/drm.ko\n"
           "kernel/drivers/gpu/drm_kms_helper.ko: kernel/drivers/gpu/drm.ko\n"
           "kernel/drivers/gpu/drm_shmem_helper.ko: kernel/drivers/gpu/drm.ko\n"
           "kernel/drivers/gpu/drm.ko:\n"
           "kernel/drivers/loop-a.ko: kernel/drivers/loop-b.ko\n"
           "kernel/drivers/loop-b.ko: kernel/drivers/loop-a.ko\n"
           "kernel/drivers/missing.ko:\n"
           "garbage line without colon\n";
    std::ofstream{dir_ / "modules.builtin"} << "kernel/net/llc/llc.ko\n"
                                               "kernel/net/bridge/bridge.ko\n";
    // Not a real module: finit_module must refuse it.
    std::ofstream{dir_ / "kernel/drivers/gpu/drm.ko"} << "not an ELF";
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
};

TEST_F(FakeModulesDir, LoadOrderPutsDependenciesFirst) {
  ModuleLoader loader{dir_};
  const auto order = loader.LoadOrder("virtio-gpu");
  ASSERT_EQ(order.size(), 4u);
  EXPECT_EQ(order.front(), "kernel/drivers/gpu/drm.ko");
  EXPECT_EQ(order.back(), "kernel/drivers/gpu/virtio-gpu.ko");
  // drm appears once even though three modules need it
  EXPECT_EQ(std::ranges::count(order, "kernel/drivers/gpu/drm.ko"), 1);
}

TEST_F(FakeModulesDir, NamesMatchWithDashOrUnderscore) {
  ModuleLoader loader{dir_};
  EXPECT_EQ(loader.LoadOrder("virtio_gpu"), loader.LoadOrder("virtio-gpu"));
  EXPECT_EQ(loader.LoadOrder("drm-kms-helper").size(), 2u);
}

TEST_F(FakeModulesDir, UnknownModuleHasNoOrder) {
  ModuleLoader loader{dir_};
  EXPECT_TRUE(loader.LoadOrder("nope").empty());
}

TEST_F(FakeModulesDir, CyclicModulesDepTerminates) {
  ModuleLoader loader{dir_};
  const auto order = loader.LoadOrder("loop-a");
  EXPECT_EQ(order.size(), 2u);
}

TEST_F(FakeModulesDir, BuiltinFromTheModulesDir) {
  ModuleLoader loader{dir_};
  EXPECT_TRUE(loader.IsBuiltin("llc"));
  EXPECT_TRUE(loader.IsBuiltin("bridge"));
  EXPECT_FALSE(loader.IsBuiltin("virtio-gpu"));
  // Built-in modules "load" without touching finit_module.
  EXPECT_TRUE(loader.Load("llc").has_value());
  EXPECT_EQ(loader.GetResults().back().status, ModuleStatus::AlreadyLoaded);
}

TEST_F(FakeModulesDir, ListOfBuiltinsLoads) {
  ModuleLoader loader{dir_};
  std::vector<ModuleLoader::ModuleConfig> modules(2);
  modules[0].name = "llc";
  modules[1].name = "bridge";
  EXPECT_TRUE(loader.Load(modules));
  EXPECT_EQ(loader.GetResults().size(), 2u);
}

// In modules.dep, but no .ko file: the open fails, and says so.
TEST_F(FakeModulesDir, MissingModuleFileSaysSo) {
  ModuleLoader loader{dir_};
  const auto r = loader.Load("missing");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, aember::utils::module::ModuleErrorCode::LoadFailed);
  EXPECT_NE(r.error().message.find("No such file"), std::string::npos)
      << r.error().message;
}

TEST_F(FakeModulesDir, UnknownModuleIsNotFound) {
  ModuleLoader loader{dir_};
  const auto r = loader.Load("nope");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, aember::utils::module::ModuleErrorCode::NotFound);
}

TEST_F(FakeModulesDir, BrokenModuleFileFailsToLoad) {
  ModuleLoader loader{dir_};
  // drm.ko is no ELF (EPERM without root, ENOEXEC/EINVAL as root);
  // its dependents fail on it first.
  const auto r = loader.Load("drm_kms_helper");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, aember::utils::module::ModuleErrorCode::LoadFailed);
  EXPECT_NE(r.error().message.find("drm.ko"), std::string::npos);
}

}  // namespace
