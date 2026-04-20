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

#include <aember-libs/utils/config/iconfig-file-parser.h>

#include <fstream>

namespace aember::utils::config {

bool IConfigFileParser::LoadJsonFromFile(
    const std::string& path,
    nlohmann::json& json,
    ConfigError& error) {

  std::ifstream file(path);
  if (!file.is_open()) {
    error = ConfigError("Failed to open file: " + path, path);
    return false;
  }

  try {
    file >> json;
  } catch (const nlohmann::json::parse_error& e) {
    error = ConfigError(
        "JSON parse error: " + std::string(e.what()), path);
    return false;
  }

  return true;
}

}