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

bool ContainersConfigParser::ParseFile(
    const std::string& path,
    config::ConfigError& error) {

  nlohmann::json json;

  if (!LoadJsonFromFile(path, json, error)) {
    return false;
  }

  containers_.clear();
  return Parse(json, error);
}

bool ContainersConfigParser::Parse(
    const nlohmann::json& json,
    config::ConfigError& error) {

  if (!json.contains("containers") ||
      !json["containers"].is_array()) {
    error = config::ConfigError("Missing 'containers'");
    return false;
  }

  for (const auto& item : json["containers"]) {
    ContainerSpec spec;

    spec.name = item["name"];
    spec.rootfs = item["rootfs"];

    containers_.push_back(std::move(spec));
  }

  return true;
}

const std::vector<ContainerSpec>&
ContainersConfigParser::GetContainers() const {
  return containers_;
}

}  // namespace aember::utils::container
