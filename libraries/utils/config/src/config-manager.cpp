/**
 * @file config-manager.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of ConfigManager for dynamic service loading.
 *
 * Provides functions to load service configurations from JSON files,
 * strings, or pre-parsed JSON objects. Validates required fields,
 * parses optional fields, and converts restart policy strings into enums.
 */

#include <aember-libs/utils/config/config-manager.h>

namespace aember::utils::config {

bool ConfigManager::Load(IConfigFileParser& parser,
                         const std::string& path) {

  ConfigError error;

  if (!parser.ParseFile(path, error)) {
    last_error_ = error;
    return false;
  }

  return true;
}

const std::optional<ConfigError>&
ConfigManager::GetLastError() const {
  return last_error_;
}

}