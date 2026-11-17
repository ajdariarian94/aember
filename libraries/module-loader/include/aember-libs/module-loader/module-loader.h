/**
 * @file module-loader.h
 * @author Arian Ajdari
 * @brief ModuleLoader — loads kernel modules at boot.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/module/module-error-code.h>
#include <aember-libs/utils/module/module-parser.h>
#include <aember-libs/utils/module/module-result.h>
#include <aember-libs/utils/module/module-status.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aember::module_loader {

class ModuleLoader {
 public:
  using ModuleResult = aember::utils::module::ModuleResult;
  using ModuleConfig = aember::utils::module::ModuleConfig;
  using ModuleStatus = aember::utils::module::ModuleStatus;
  using ModuleErrorCode = aember::utils::module::ModuleErrorCode;
  using ModulesConfigParser = aember::utils::module::ModulesConfigParser;
  using Logger = aember::utils::logging::Logger;

  struct Error {
    std::string message;
    ModuleErrorCode code{ModuleErrorCode::None};
  };

  template <typename T = void>
  using Result = std::expected<T, Error>;

  /** @param modules_dir directory with modules.dep / modules.builtin and
   *         the .ko files; default /lib/modules/<uname -r>. */
  explicit ModuleLoader(std::filesystem::path modules_dir = {});
  ~ModuleLoader() = default;

  ModuleLoader(const ModuleLoader&) = delete;
  ModuleLoader& operator=(const ModuleLoader&) = delete;

  // ---------------------------------------------------------------------------
  // Module loading
  // ---------------------------------------------------------------------------

  /** Load a single module by name. */
  Result<> Load(std::string_view name);

  /** Bulk load — returns false if any module failed. */
  bool Load(const std::vector<ModuleConfig>& modules);

  /** Parse module configs from a JSON file. */
  std::vector<ModuleConfig> LoadModules(std::string_view path);

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  [[nodiscard]] bool IsLoaded(std::string_view name) const;

  /** True if the module is built into the running kernel
   *  (listed in <modules_dir>/modules.builtin). */
  [[nodiscard]] bool IsBuiltin(std::string_view name) const;

  /** .ko files (relative to modules_dir) to load for @p name, its
   *  dependencies first, from modules.dep. Empty if the module is unknown.
   */
  [[nodiscard]] std::vector<std::string> LoadOrder(std::string_view name);

  [[nodiscard]] const std::filesystem::path& ModulesDir() const {
    return modules_dir_;
  }

  [[nodiscard]] const std::vector<ModuleResult>& GetResults() const {
    return results_;
  }

 private:
  /** finit_module(2) one .ko: 0, or -errno (-EEXIST: already loaded). */
  static int LoadFile(const std::filesystem::path& ko);

  /** modules.dep parsed once: normalized module name -> its line. */
  struct DepEntry {
    std::string path;               ///< relative .ko path
    std::vector<std::string> deps;  ///< relative .ko paths it needs
  };
  const std::unordered_map<std::string, DepEntry>& DepMap();

  std::filesystem::path modules_dir_;
  std::optional<std::unordered_map<std::string, DepEntry>> dep_map_;

  std::vector<ModuleResult> results_;

  ModulesConfigParser parser_{};

  mutable Logger log_{"module-loader"};
};

}  // namespace aember::module_loader
