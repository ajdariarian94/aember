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

#include <aember-libs/config-manager/config-manager.h>

#include <fstream>
#include <sstream>

namespace aember::config_manager {

/**
 * @brief Construct a new ConfigManager and initialize logger.
 */
ConfigManager::ConfigManager() : log_("config-manager") {
  log_.info("ConfigManager initialized");
}

/**
 * @brief Destroy the ConfigManager.
 */
ConfigManager::~ConfigManager() = default;

/**
 * @brief Load configuration from a JSON file.
 * @param path Path to the configuration file
 * @return true if successfully loaded, false otherwise
 */
bool ConfigManager::LoadFromFile(const std::string& path) {
  log_.info("Loading configuration from file: {}", path);

  Clear();

  std::ifstream file(path);
  if (!file.is_open()) {
    SetError(ConfigError("Failed to open config file: " + path, path));
    return false;
  }

  nlohmann::json config;
  try {
    file >> config;
  } catch (const nlohmann::json::parse_error& e) {
    SetError(ConfigError("JSON parse error: " + std::string(e.what()), path));
    return false;
  }

  return LoadFromJson(config);
}

/**
 * @brief Load configuration from a JSON string.
 * @param json_str JSON string containing configuration
 * @return true if successfully loaded, false otherwise
 */
bool ConfigManager::LoadFromString(const std::string& json_str) {
  log_.info("Loading configuration from string");

  Clear();

  nlohmann::json config;
  try {
    config = nlohmann::json::parse(json_str);
  } catch (const nlohmann::json::parse_error& e) {
    SetError(ConfigError("JSON parse error: " + std::string(e.what())));
    return false;
  }

  return LoadFromJson(config);
}

/**
 * @brief Load configuration from a parsed JSON object.
 * @param config JSON object containing configuration
 * @return true if successfully loaded, false otherwise
 */
bool ConfigManager::LoadFromJson(const nlohmann::json& config) {
  log_.info("Loading configuration from JSON object");

  Clear();

  if (!config.is_object()) {
    SetError("Configuration must be a JSON object");
    return false;
  }

  if (config.contains("services")) {
    if (!ParseServices(config["services"])) { return false; }
  }

  loaded_ = true;
  log_.info("Configuration loaded successfully ({} services)",
            services_.size());
  return true;
}

/**
 * @brief Parse the "services" array in the configuration.
 * @param json JSON array of service objects
 * @return true if successfully parsed, false otherwise
 */
bool ConfigManager::ParseServices(const nlohmann::json& json) {
  if (!json.is_array()) {
    SetError("'services' must be an array");
    return false;
  }

  for (size_t i = 0; i < json.size(); ++i) {
    const auto& service_json = json[i];

    if (!service_json.is_object()) {
      SetError(ConfigError("Service at index " + std::to_string(i) +
                           " must be an object"));
      return false;
    }

    aember::service_manager::ServiceConfig config;
    if (!ParseService(service_json, config)) { return false; }

    services_.push_back(config);
  }

  return true;
}

/**
 * @brief Parse a single service configuration entry.
 * @param service_json JSON object representing the service
 * @param config Output ServiceConfig object
 * @return true if successfully parsed, false otherwise
 */
bool ConfigManager::ParseService(
    const nlohmann::json& service_json,
    aember::service_manager::ServiceConfig& config) {
  // Required fields: name
  if (!service_json.contains("name")) {
    SetError("Service missing required field: 'name'");
    return false;
  }
  if (!service_json["name"].is_string()) {
    SetError("Service 'name' must be a string");
    return false;
  }
  config.name = service_json["name"].get<std::string>();
  if (config.name.empty()) {
    SetError("Service 'name' cannot be empty");
    return false;
  }

  // Required fields: command
  if (!service_json.contains("command")) {
    SetError("Service missing required field: 'command'");
    return false;
  }
  if (!service_json["command"].is_string()) {
    SetError("Service 'command' must be a string");
    return false;
  }
  config.command = service_json["command"].get<std::string>();
  if (config.command.empty()) {
    SetError("Service 'command' cannot be empty");
    return false;
  }

  // Optional fields

  // Args
  if (service_json.contains("args")) {
    if (!service_json["args"].is_array()) {
      SetError("Service 'args' must be an array");
      return false;
    }

    for (const auto& arg : service_json["args"]) {
      if (!arg.is_string()) {
        SetError("Service 'args' must contain only strings");
        return false;
      }
      config.args.push_back(arg.get<std::string>());
    }
  }

  // Environment
  if (service_json.contains("environment")) {
    if (!service_json["environment"].is_object()) {
      SetError("Service 'environment' must be an object");
      return false;
    }

    for (auto it = service_json["environment"].begin();
         it != service_json["environment"].end();
         ++it) {
      if (!it.value().is_string()) {
        SetError("Service 'environment' values must be strings");
        return false;
      }
      config.environment[it.key()] = it.value().get<std::string>();
    }
  }

  // Working directory
  if (service_json.contains("working_directory")) {
    if (!service_json["working_directory"].is_string()) {
      SetError("Service 'working_directory' must be a string");
      return false;
    }
    config.working_directory =
        service_json["working_directory"].get<std::string>();
  }

  // Restart policy
  if (service_json.contains("restart_policy")) {
    if (!service_json["restart_policy"].is_string()) {
      SetError("Service 'restart_policy' must be a string");
      return false;
    }
    config.restart_policy =
        ParseRestartPolicy(service_json["restart_policy"].get<std::string>());
  }

  // Dependencies
  if (service_json.contains("dependencies")) {
    if (!service_json["dependencies"].is_array()) {
      SetError("Service 'dependencies' must be an array");
      return false;
    }

    for (const auto& dep : service_json["dependencies"]) {
      if (!dep.is_string()) {
        SetError("Service 'dependencies' must contain only strings");
        return false;
      }
      config.dependencies.push_back(dep.get<std::string>());
    }
  }

  // Max restart attempts
  if (service_json.contains("max_restart_attempts")) {
    if (!service_json["max_restart_attempts"].is_number_integer()) {
      SetError("Service 'max_restart_attempts' must be an integer");
      return false;
    }
    int attempts = service_json["max_restart_attempts"].get<int>();
    if (attempts < 0) {
      SetError("Service 'max_restart_attempts' must be >= 0");
      return false;
    }
    config.max_restart_attempts = attempts;
  }

  // Restart delay in seconds
  if (service_json.contains("restart_delay_seconds")) {
    if (!service_json["restart_delay_seconds"].is_number()) {
      SetError("Service 'restart_delay_seconds' must be a number");
      return false;
    }
    int delay = service_json["restart_delay_seconds"].get<int>();
    if (delay < 0) {
      SetError("Service 'restart_delay_seconds' must be >= 0");
      return false;
    }
    config.restart_delay = std::chrono::seconds(delay);
  }

  return true;
}

/**
 * @brief Convert restart policy string to enum.
 */
aember::service_manager::RestartPolicy ConfigManager::ParseRestartPolicy(
    const std::string& policy_str) {
  if (policy_str == "never") {
    return aember::service_manager::RestartPolicy::NEVER;
  } else if (policy_str == "on-failure" || policy_str == "on_failure") {
    return aember::service_manager::RestartPolicy::ON_FAILURE;
  } else if (policy_str == "always") {
    return aember::service_manager::RestartPolicy::ALWAYS;
  } else {
    log_.warn("Unknown restart policy '{}', defaulting to 'never'", policy_str);
    return aember::service_manager::RestartPolicy::NEVER;
  }
}

/**
 * @brief Return all loaded service configurations.
 */
std::vector<aember::service_manager::ServiceConfig> ConfigManager::GetServices()
    const {
  return services_;
}

/**
 * @brief Return a specific service configuration by name.
 */
std::optional<aember::service_manager::ServiceConfig> ConfigManager::GetService(
    const std::string& name) const {
  for (const auto& service : services_) {
    if (service.name == name) { return service; }
  }
  return std::nullopt;
}

/**
 * @brief Clear loaded configuration and reset state.
 */
void ConfigManager::Clear() {
  services_.clear();
  loaded_ = false;
  last_error_.reset();
}

/**
 * @brief Set last error from a message string.
 */
void ConfigManager::SetError(const std::string& message) {
  last_error_ = ConfigError(message);
  log_.error("{}", message);
}

/**
 * @brief Set last error from a ConfigError object.
 */
void ConfigManager::SetError(const ConfigError& error) {
  last_error_ = error;
  log_.error("{}", error.message);
}

/**
 * @brief Validate a JSON configuration file without loading.
 */
bool ConfigManager::ValidateFile(const std::string& path, ConfigError* error) {
  std::ifstream file(path);
  if (!file.is_open()) {
    if (error) {
      *error = ConfigError("Failed to open config file: " + path, path);
    }
    return false;
  }

  nlohmann::json config;
  try {
    file >> config;
  } catch (const nlohmann::json::parse_error& e) {
    if (error) {
      *error = ConfigError("JSON parse error: " + std::string(e.what()), path);
    }
    return false;
  }

  return ValidateJson(config, error);
}

/**
 * @brief Validate a parsed JSON object without loading.
 */
bool ConfigManager::ValidateJson(const nlohmann::json& config,
                                 ConfigError* error) {
  if (!config.is_object()) {
    if (error) { *error = ConfigError("Configuration must be a JSON object"); }
    return false;
  }

  ConfigManager temp;
  bool result = temp.LoadFromJson(config);

  if (!result && error && temp.GetLastError()) {
    *error = *temp.GetLastError();
  }

  return result;
}

}  // namespace aember::config_manager
