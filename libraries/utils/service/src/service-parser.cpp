/**
 * @file service-config.cpp
 * @author Arian Ajdari
 * @brief Library implementation for service config
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/service/service-parser.h>

namespace aember::utils::service {

bool ServicesConfigParser::ParseFile(
    const std::string& path,
    config::ConfigError& error) {

  nlohmann::json json;

  if (!LoadJsonFromFile(path, json, error)) {
    return false;
  }

  services_.clear();
  return Parse(json, error);
}

bool ServicesConfigParser::Parse(
    const nlohmann::json& json,
    config::ConfigError& error) {

  if (!json.is_object() || !json.contains("services")) {
    error = config::ConfigError("Missing or invalid 'services'");
    return false;
  }

  if (!json["services"].is_array()) {
    error = config::ConfigError("'services' must be an array");
    return false;
  }

  for (const auto& item : json["services"]) {
    ServiceConfig svc;

    if (!ParseService(item, svc, error)) {
      return false;
    }

    services_.push_back(std::move(svc));
  }

  return true;
}

bool ServicesConfigParser::ParseService(
    const nlohmann::json& json,
    ServiceConfig& svc,
    config::ConfigError& error) {

  // -------------------------
  // NAME (required)
  // -------------------------
  if (!json.contains("name") || !json["name"].is_string()) {
    error = config::ConfigError("Service missing 'name'");
    return false;
  }
  svc.name = json["name"].get<std::string>();

  // -------------------------
  // TYPE
  // -------------------------
  std::string type = json.value("type", "process");

  if (type == "container") {
   
  }
  else {
    svc.type = ServiceType::PROCESS;

    if (!json.contains("command") ||
        !json["command"].is_string()) {
      error = config::ConfigError("Process service missing 'command'");
      return false;
    }

    svc.command = json["command"].get<std::string>();
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
      svc.args.push_back(arg.get<std::string>());
    }
  }

  // -------------------------
  // WORKING DIRECTORY (optional)
  // -------------------------
  if (json.contains("working_directory")) {
    if (!json["working_directory"].is_string()) {
      error = config::ConfigError("'working_directory' must be string");
      return false;
    }
    svc.working_directory = json["working_directory"].get<std::string>();
  }

  // -------------------------
  // ENVIRONMENT (optional)
  // -------------------------
  if (json.contains("environment")) {
    if (!json["environment"].is_object()) {
      error = config::ConfigError("'environment' must be object");
      return false;
    }

    for (auto it = json["environment"].begin();
         it != json["environment"].end(); ++it) {

      if (!it.value().is_string()) {
        error = config::ConfigError("Environment values must be strings");
        return false;
      }

      svc.environment[it.key()] = it.value().get<std::string>();
    }
  }

  // -------------------------
  // RESTART POLICY (optional)
  // -------------------------
  if (json.contains("restart_policy")) {
    if (!json["restart_policy"].is_string()) {
      error = config::ConfigError("'restart_policy' must be string");
      return false;
    }

    svc.restart_policy =
        ParseRestartPolicy(json["restart_policy"].get<std::string>());
  }

  // -------------------------
  // DEPENDENCIES (optional)
  // -------------------------
  if (json.contains("dependencies")) {
    if (!json["dependencies"].is_array()) {
      error = config::ConfigError("'dependencies' must be array");
      return false;
    }

    for (const auto& dep : json["dependencies"]) {
      if (!dep.is_string()) {
        error = config::ConfigError("Dependencies must be strings");
        return false;
      }
      svc.dependencies.push_back(dep.get<std::string>());
    }
  }

  // -------------------------
  // MAX RESTART ATTEMPTS (optional)
  // -------------------------
  if (json.contains("max_restart_attempts")) {
    if (!json["max_restart_attempts"].is_number_integer()) {
      error = config::ConfigError("'max_restart_attempts' must be integer");
      return false;
    }

    int attempts = json["max_restart_attempts"].get<int>();

    if (attempts < 0) {
      error = config::ConfigError("'max_restart_attempts' must be >= 0");
      return false;
    }

    svc.max_restart_attempts = attempts;
  }
  else {
    svc.max_restart_attempts = 0;
  }

  // -------------------------
  // RESTART DELAY (optional)
  // -------------------------
  if (json.contains("restart_delay")) {
    if (!json["restart_delay"].is_number_integer()) {
      error = config::ConfigError("'restart_delay' must be integer");
      return false;
    }

    int delay = json["restart_delay"].get<int>();

    if (delay < 0) {
      error = config::ConfigError("'restart_delay' must be >= 0");
      return false;
    }

    svc.restart_delay = std::chrono::seconds(delay);
  }
  else {
    svc.restart_delay = std::chrono::seconds(0);
  }

  return true;
}

RestartPolicy ServicesConfigParser::ParseRestartPolicy(
    const std::string& policy) {

  if (policy == "always") {
    return RestartPolicy::ALWAYS;
  }

  if (policy == "on-failure" || policy == "on_failure") {
    return RestartPolicy::ON_FAILURE;
  }

  if (policy == "never") {
    return RestartPolicy::NEVER;
  }

  return RestartPolicy::NEVER;
}

const std::vector<ServiceConfig>&
ServicesConfigParser::GetServices() const {
  return services_;
}

} // namespace aember::utils::service
