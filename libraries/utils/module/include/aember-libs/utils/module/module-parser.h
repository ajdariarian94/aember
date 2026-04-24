/**
 * @file module-error-code.h
 * @author Arian Ajdari
 * @brief Enum class for MountPoint
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/config/iconfig-file-parser.h>
#include <aember-libs/utils/module/module-config.h>

#include <string>
#include <vector>

namespace aember::utils::module {

class ModulesConfigParser : public config::IConfigFileParser {
 public:
  bool ParseFile(const std::string& path, config::ConfigError& error) override;

  const std::vector<ModuleConfig>& GetModules() const;

 private:
  bool Parse(const nlohmann::json& json, config::ConfigError& error);

  bool ParseModule(const nlohmann::json& json, ModuleConfig& cfg,
                   config::ConfigError& error);

 private:
  std::vector<ModuleConfig> modules_;
};

}  // namespace aember::utils::module
