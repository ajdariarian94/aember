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

#include <string>
#include <nlohmann/json.hpp>

#include <aember-libs/utils/config/config-error.h>

namespace aember::utils::config {

class IConfigFileParser {
public:
  virtual ~IConfigFileParser() = default;

  virtual bool ParseFile(const std::string& path,
                         ConfigError& error) = 0;

protected:
  bool LoadJsonFromFile(const std::string& path,
                        nlohmann::json& json,
                        ConfigError& error);
};

}
