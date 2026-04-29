/**
 * @file module-loader.h
 * @author Arian Ajdari
 * @brief Library definition for ModuleLoader - loads kernel modules at boot.
 * @version 0.1
 * @date 2026-03-29
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/module/module-error-code.h>
#include <aember-libs/utils/module/module-parser.h>
#include <aember-libs/utils/module/module-result.h>
#include <aember-libs/utils/module/module-status.h>

#include <string>
#include <vector>

namespace aember::module_loader {

class ModuleLoader {
 public:
  using ModuleResult = aember::utils::module::ModuleResult;
  using ModuleConfig = aember::utils::module::ModuleConfig;
  using ModuleStatus = aember::utils::module::ModuleStatus;
  using ModuleErrorCode = aember::utils::module::ModuleErrorCode;
  using Logger = aember::utils::logging::Logger;
  using ModulesConfigParser = aember::utils::module::ModulesConfigParser;

  ModuleLoader();
  ~ModuleLoader() = default;

  ModuleLoader(const ModuleLoader&) = delete;
  ModuleLoader& operator=(const ModuleLoader&) = delete;

  // --------------------------
  // Module loading
  // --------------------------

  bool Load(const std::string& name);
  bool Load(const std::vector<ModuleConfig>& modules);

  /**
   * @brief Parse + return module configs (same style as ContainerManager)
   */
  std::vector<ModuleConfig> LoadModules(const std::string& path);

  // --------------------------
  // Queries
  // --------------------------

  bool IsLoaded(const std::string& name) const;

  const std::vector<ModuleResult>& GetResults() const { return results_; }

 private:
  int RunModprobe(const std::string& name);

 private:
  std::vector<ModuleResult> results_;
  Logger log_{"module-loader"};

  ModulesConfigParser parser_{};  // 👈 same pattern as container manager
};

}  // namespace aember::module_loader
