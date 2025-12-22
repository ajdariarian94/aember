#pragma once

#include <aember-libs/service-manager/service.h>
#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace aember::config_manager {

struct ConfigError {
  std::string message;
  std::string file;
  int line = -1;

  ConfigError() = default;

  ConfigError(const std::string& msg) : message(msg) {}
  ConfigError(const std::string& msg, const std::string& f, int l = -1)
      : message(msg), file(f), line(l) {}
};

class ConfigManager {
 public:
  ConfigManager();
  ~ConfigManager();

  // Non-copyable
  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;

  // Load configuration from file
  bool LoadFromFile(const std::string& path);

  // Load configuration from JSON string
  bool LoadFromString(const std::string& json_str);

  // Load configuration from JSON object
  bool LoadFromJson(const nlohmann::json& config);

  // Get all service configurations
  std::vector<aember::service_manager::ServiceConfig> GetServices() const;

  // Get a specific service configuration
  std::optional<aember::service_manager::ServiceConfig> GetService(
      const std::string& name) const;

  // Check if configuration has been loaded
  bool IsLoaded() const { return loaded_; }

  // Get last error
  std::optional<ConfigError> GetLastError() const { return last_error_; }

  // Clear configuration
  void Clear();

  // Validate configuration without loading
  static bool ValidateFile(const std::string& path,
                           ConfigError* error = nullptr);
  static bool ValidateJson(const nlohmann::json& config,
                           ConfigError* error = nullptr);

 private:
  bool ParseServices(const nlohmann::json& json);
  bool ParseService(const nlohmann::json& service_json,
                    aember::service_manager::ServiceConfig& config);

  aember::service_manager::RestartPolicy ParseRestartPolicy(
      const std::string& policy_str);

  void SetError(const std::string& message);
  void SetError(const ConfigError& error);

  std::vector<aember::service_manager::ServiceConfig> services_;
  bool loaded_ = false;
  std::optional<ConfigError> last_error_;
  mutable aember::utils::Logger log_;
};

}  // namespace aember::config_manager
