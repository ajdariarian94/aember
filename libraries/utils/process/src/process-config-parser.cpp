/**
 * @file process-config-parser.cpp
 * @author Arian Ajdari
 * @brief ProcessConfigParser implementation.
 *        Adapted from utils/service/service-parser.cpp — process fields only,
 *        no ServiceType, no RestartPolicy enum.
 * @version 0.1
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/process/process-config-parser.h>

namespace aember::utils::process {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ProcessConfigParser::ParseFile(const std::string& path,
                                    config::ConfigError& error) {
  nlohmann::json json;

  if (!LoadJsonFromFile(path, json, error)) { return false; }

  processes_.clear();
  return Parse(json, error);
}

const std::vector<ProcessConfig>& ProcessConfigParser::GetProcesses() const {
  return processes_;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool ProcessConfigParser::Parse(const nlohmann::json& json,
                                config::ConfigError& error) {
  if (!json.is_object() || !json.contains("processes")) {
    error = config::ConfigError("Missing or invalid 'processes' array");
    return false;
  }

  if (!json["processes"].is_array()) {
    error = config::ConfigError("'processes' must be an array");
    return false;
  }

  for (const auto& item : json["processes"]) {
    ProcessConfig cfg;

    if (!ParseProcess(item, cfg, error)) { return false; }

    processes_.push_back(std::move(cfg));
  }

  return true;
}

bool ProcessConfigParser::ParseProcess(const nlohmann::json& json,
                                       ProcessConfig& cfg,
                                       config::ConfigError& error) {
  // -------------------------
  // NAME (required)
  // -------------------------
  if (!json.contains("name") || !json["name"].is_string()) {
    error = config::ConfigError("Process entry missing 'name'");
    return false;
  }
  cfg.name = json["name"].get<std::string>();

  // -------------------------
  // EXECUTABLE (required)
  // -------------------------
  if (!json.contains("executable") || !json["executable"].is_string()) {
    error =
        config::ConfigError("Process '" + cfg.name + "' missing 'executable'");
    return false;
  }
  cfg.executable = json["executable"].get<std::string>();

  // -------------------------
  // DESCRIPTION (optional)
  // -------------------------
  if (json.contains("description")) {
    if (!json["description"].is_string()) {
      error = config::ConfigError("'description' must be a string");
      return false;
    }
    cfg.description = json["description"].get<std::string>();
  }

  // -------------------------
  // ARGS (optional)
  // -------------------------
  if (json.contains("args")) {
    if (!json["args"].is_array()) {
      error = config::ConfigError("'args' must be an array");
      return false;
    }

    for (const auto& arg : json["args"]) {
      if (!arg.is_string()) {
        error = config::ConfigError("All 'args' must be strings");
        return false;
      }
      cfg.args.push_back(arg.get<std::string>());
    }
  }

  // -------------------------
  // WORKING DIRECTORY (optional)
  // -------------------------
  if (json.contains("working_directory")) {
    if (!json["working_directory"].is_string()) {
      error = config::ConfigError("'working_directory' must be a string");
      return false;
    }
    cfg.working_directory = json["working_directory"].get<std::string>();
  }

  // -------------------------
  // ENVIRONMENT (optional)
  // -------------------------
  if (json.contains("environment")) {
    if (!json["environment"].is_object()) {
      error = config::ConfigError("'environment' must be an object");
      return false;
    }

    for (auto it = json["environment"].begin(); it != json["environment"].end();
         ++it) {
      if (!it.value().is_string()) {
        error = config::ConfigError("Environment values must be strings");
        return false;
      }
      cfg.environment[it.key()] = it.value().get<std::string>();
    }
  }

  // -------------------------
  // DEPENDENCIES (optional)
  // -------------------------
  if (json.contains("dependencies")) {
    if (!json["dependencies"].is_array()) {
      error = config::ConfigError("'dependencies' must be an array");
      return false;
    }

    for (const auto& dep : json["dependencies"]) {
      if (!dep.is_string()) {
        error = config::ConfigError("Dependencies must be strings");
        return false;
      }
      cfg.dependencies.push_back(dep.get<std::string>());
    }
  }

  // -------------------------
  // RESTART ON FAILURE (optional, default: true)
  // -------------------------
  if (json.contains("restart_on_failure")) {
    if (!json["restart_on_failure"].is_boolean()) {
      error = config::ConfigError("'restart_on_failure' must be a boolean");
      return false;
    }
    cfg.restart_on_failure = json["restart_on_failure"].get<bool>();
  }

  // -------------------------
  // MAX RESTARTS (optional, default: 0 = unlimited)
  // -------------------------
  if (json.contains("max_restarts")) {
    if (!json["max_restarts"].is_number_unsigned()) {
      error = config::ConfigError("'max_restarts' must be an unsigned integer");
      return false;
    }

    cfg.max_restarts = json["max_restarts"].get<unsigned int>();
  }

  // -------------------------
  // RESTART DELAY ms (optional, default: 1000)
  // -------------------------
  if (json.contains("restart_delay_ms")) {
    if (!json["restart_delay_ms"].is_number_unsigned()) {
      error =
          config::ConfigError("'restart_delay_ms' must be an unsigned integer");
      return false;
    }

    cfg.restart_delay =
        std::chrono::milliseconds(json["restart_delay_ms"].get<unsigned int>());
  }

  // -------------------------
  // STOP TIMEOUT ms (optional, default: 5000)
  // -------------------------
  if (json.contains("stop_timeout_ms")) {
    if (!json["stop_timeout_ms"].is_number_unsigned()) {
      error =
          config::ConfigError("'stop_timeout_ms' must be an unsigned integer");
      return false;
    }

    cfg.stop_timeout =
        std::chrono::milliseconds(json["stop_timeout_ms"].get<unsigned int>());
  }

  return true;
}

}  // namespace aember::utils::process
