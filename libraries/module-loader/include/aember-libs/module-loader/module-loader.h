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
#include <string>
#include <string_view>
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

  ModuleLoader();
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

  [[nodiscard]] const std::vector<ModuleResult>& GetResults() const {
    return results_;
  }

 private:
  /** Fork + exec modprobe. Returns the exit code, or -1 on fork/wait failure.
   */
  int RunModprobe(std::string_view name);

  std::vector<ModuleResult> results_;

  ModulesConfigParser parser_{};

  mutable Logger log_{"module-loader"};
};

}  // namespace aember::module_loader
