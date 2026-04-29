/**
 * @file config-manager.h
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Configuration loader and validator for service definitions.
 *
 * The ConfigManager is responsible for loading, validating, and parsing
 * service configuration files. It converts JSON-based configuration data
 * into ServiceConfig objects consumable by the ServiceManager.
 *
 * This component performs no service lifecycle actions; it only provides
 * validated configuration data.
 */

#pragma once

#include <memory>
#include <optional>

#include <aember-libs/utils/config/config-error.h>
#include <aember-libs/utils/config/iconfig-file-parser.h>

namespace aember::utils::config {

class ConfigManager {
 public:
  bool Load(IConfigFileParser& parser, const std::string& path);

  const std::optional<ConfigError>& GetLastError() const;

 private:
  std::optional<ConfigError> last_error_;
};

}  // namespace aember::utils::config
