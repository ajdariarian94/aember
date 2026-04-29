/**
 * @file container-parser.cpp
 * @author Arian Ajdari
 * @brief Library implementation for restart policy
 * @version 0.1
 * @date 2026-04-11
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/container/container-parser.h>

namespace aember::utils::container {

bool ContainersConfigParser::ParseFile(const std::string& path,
                                       config::ConfigError& error) {
  nlohmann::json json;

  if (!LoadJsonFromFile(path, json, error)) { return false; }

  containers_.clear();
  return Parse(json, error);
}

bool ContainersConfigParser::Parse(const nlohmann::json& json,
                                   config::ConfigError& error) {
  if (!json.is_object() || !json.contains("containers")) {
    error = config::ConfigError("Missing 'containers'");
    return false;
  }

  if (!json["containers"].is_array()) {
    error = config::ConfigError("'containers' must be an array");
    return false;
  }

  for (const auto& item : json["containers"]) {
    ContainerConfig cfg;

    if (!ParseContainer(item, cfg, error)) { return false; }

    containers_.push_back(std::move(cfg));
  }

  return true;
}

bool ContainersConfigParser::ParseContainer(const nlohmann::json& json,
                                            ContainerConfig& cfg,
                                            config::ConfigError& error) {
  // -------------------------
  // NAME (required)
  // -------------------------
  if (!json.contains("name") || !json["name"].is_string()) {
    error = config::ConfigError("Container missing 'name'");
    return false;
  }

  cfg.name = json["name"].get<std::string>();

  // -------------------------
  // CONTAINER OBJECT (required)
  // -------------------------
  if (!json.contains("container") || !json["container"].is_object()) {
    error = config::ConfigError("Missing 'container' object");
    return false;
  }

  const auto& ctr = json["container"];

  // squashfs
  if (!ctr.contains("squashfs") || !ctr["squashfs"].is_string()) {
    error = config::ConfigError("Missing 'container.squashfs'");
    return false;
  }

  cfg.squashfs = ctr["squashfs"].get<std::string>();

  // rootfs
  if (!ctr.contains("rootfs") || !ctr["rootfs"].is_string()) {
    error = config::ConfigError("Missing 'container.rootfs'");
    return false;
  }

  cfg.rootfs = ctr["rootfs"].get<std::string>();

  // args (optional)
  if (ctr.contains("args")) {
    if (!ctr["args"].is_array()) {
      error = config::ConfigError("'container.args' must be array");
      return false;
    }

    for (const auto& arg : ctr["args"]) {
      if (!arg.is_string()) {
        error = config::ConfigError("Container args must be strings");
        return false;
      }

      cfg.args.push_back(arg.get<std::string>());
    }
  }

  // -------------------------
  // IGNORE service-level fields intentionally
  // -------------------------
  // restart_policy
  // dependencies
  // max_restart_attempts
  // restart_delay

  return true;
}

const std::vector<ContainerConfig>& ContainersConfigParser::GetContainers()
    const {
  return containers_;
}

}  // namespace aember::utils::container
