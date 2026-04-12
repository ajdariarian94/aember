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

#include <aember-libs/service-manager/service.h>
#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace aember::config_manager {

/**
 * @brief Describes a configuration parsing or validation error.
 *
 * Used to report human-readable error information when loading or
 * validating configuration data.
 */
struct ConfigError {
  std::string message;  // Error description
  std::string file;     // Source file (if applicable)
  int line = -1;        // Line number (if available)

  ConfigError() = default;

  explicit ConfigError(const std::string& msg) : message(msg) {}
  ConfigError(const std::string& msg, const std::string& f, int l = -1)
      : message(msg), file(f), line(l) {}
};

/**
 * @brief Loads and validates service configuration data.
 *
 * ConfigManager supports loading configuration from files, strings,
 * or pre-parsed JSON objects. Successfully loaded configurations
 * can be queried for individual or complete service definitions.
 *
 * This class is intentionally non-copyable to prevent accidental
 * duplication of configuration state.
 */
class ConfigManager {
 public:
  ConfigManager();
  ~ConfigManager();

  // Non-copyable
  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;

  /**
   * @brief Load configuration from a JSON file.
   *
   * @param path Path to the configuration file.
   * @return true on success, false on failure.
   */
  bool LoadFromFile(const std::string& path);

  /**
   * @brief Load configuration from a JSON string.
   *
   * @param json_str Raw JSON string.
   * @return true on success, false on failure.
   */
  bool LoadFromString(const std::string& json_str);

  /**
   * @brief Load configuration from a parsed JSON object.
   *
   * @param config Parsed JSON configuration.
   * @return true on success, false on failure.
   */
  bool LoadFromJson(const nlohmann::json& config);

  /**
   * @brief Retrieve all loaded service configurations.
   *
   * @return Vector of ServiceConfig objects.
   */
  std::vector<aember::service_manager::ServiceConfig> GetServices() const;

  /**
   * @brief Retrieve a specific service configuration by name.
   *
   * @param name Service name.
   * @return ServiceConfig if found, std::nullopt otherwise.
   */
  std::optional<aember::service_manager::ServiceConfig> GetService(
      const std::string& name) const;

  /**
   * @brief Check whether a configuration has been successfully loaded.
   */
  bool IsLoaded() const { return loaded_; }

  /**
   * @brief Retrieve the last configuration error, if any.
   */
  std::optional<ConfigError> GetLastError() const { return last_error_; }

  /**
   * @brief Clear all loaded configuration data.
   */
  void Clear();

  /**
   * @brief Validate a configuration file without loading it.
   *
   * @param path Path to configuration file.
   * @param error Optional error output.
   * @return true if valid, false otherwise.
   */
  static bool ValidateFile(const std::string& path,
                           ConfigError* error = nullptr);

  /**
   * @brief Validate a parsed JSON configuration without loading it.
   *
   * @param config Parsed JSON object.
   * @param error Optional error output.
   * @return true if valid, false otherwise.
   */
  static bool ValidateJson(const nlohmann::json& config,
                           ConfigError* error = nullptr);

 private:
  /**
   * @brief Parse the "services" section of the configuration.
   */
  bool ParseServices(const nlohmann::json& json);

  /**
   * @brief Parse a single service entry.
   */
  bool ParseService(const nlohmann::json& service_json,
                    aember::service_manager::ServiceConfig& config);

  /**
   * @brief Convert restart policy string to enum.
   */
  aember::utils::service::RestartPolicy ParseRestartPolicy(
      const std::string& policy_str);

  /**
   * @brief Store an error message and mark configuration as invalid.
   */
  void SetError(const std::string& message);

  /**
   * @brief Store a detailed error object.
   */
  void SetError(const ConfigError& error);

  std::vector<aember::service_manager::ServiceConfig>
      services_;                                // Loaded services
  bool loaded_ = false;                         // Load state
  std::optional<ConfigError> last_error_;       // Last error
  mutable aember::utils::logging::Logger log_;  // Logger
};

}  // namespace aember::config_manager
