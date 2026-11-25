/**
 * @file container-parser.cpp
 * @author Arian Ajdari
 * @brief Library implementation for ContainersConfigParser
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/container-parser.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace aember::containerd {

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

namespace {

/// A whole number that fits an unsigned. get<unsigned>() alone wraps bigger
/// ones: "max_restart_attempts": 4294967296 would be 0 (= unlimited).
bool FitsUnsigned(const nlohmann::json& value) {
  return value.is_number_unsigned() &&
         value.get<std::uint64_t>() <= std::numeric_limits<unsigned>::max();
}

}  // namespace

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

  // squashfs (required)
  if (!ctr.contains("squashfs") || !ctr["squashfs"].is_string()) {
    error = config::ConfigError("Missing 'container.squashfs'");
    return false;
  }
  cfg.squashfs = ctr["squashfs"].get<std::string>();

  // rootfs (required)
  if (!ctr.contains("rootfs") || !ctr["rootfs"].is_string()) {
    error = config::ConfigError("Missing 'container.rootfs'");
    return false;
  }
  cfg.rootfs = ctr["rootfs"].get<std::string>();

  // config_path (required) — path to lxc.container.conf
  if (!ctr.contains("config_path") || !ctr["config_path"].is_string()) {
    error = config::ConfigError(
        "Missing 'container.config_path' — lxc.container.conf path is "
        "required");
    return false;
  }
  cfg.config_path = ctr["config_path"].get<std::string>();

  // args (optional)
  if (ctr.contains("args")) {
    if (!ctr["args"].is_array()) {
      error = config::ConfigError("'container.args' must be an array");
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
  // Lifecycle (optional, top level)
  // -------------------------
  const auto seconds = [&](const char* key, std::chrono::seconds& out) {
    if (!json.contains(key)) return true;
    if (!FitsUnsigned(json[key])) {
      error = config::ConfigError(std::string{"'"} + key +
                                  "' must be a number of seconds");
      return false;
    }
    out = std::chrono::seconds{json[key].get<unsigned>()};
    return true;
  };

  if (json.contains("init")) {
    if (!json["init"].is_boolean()) {
      error = config::ConfigError("'init' must be true or false");
      return false;
    }
    cfg.init = json["init"].get<bool>();
  }
  if (!seconds("stop_timeout", cfg.stop_timeout)) return false;
  if (json.contains("restart_policy")) {
    const auto policy =
        json["restart_policy"].is_string()
            ? RestartPolicyFromString(json["restart_policy"].get<std::string>())
            : std::nullopt;
    if (!policy) {
      error = config::ConfigError(
          "'restart_policy' must be \"no\", \"on-failure\" or \"always\"");
      return false;
    }
    cfg.restart_policy = *policy;
  }
  if (json.contains("max_restart_attempts")) {
    if (!FitsUnsigned(json["max_restart_attempts"])) {
      error = config::ConfigError("'max_restart_attempts' must be >= 0");
      return false;
    }
    cfg.max_restart_attempts = json["max_restart_attempts"].get<unsigned>();
  }
  if (!seconds("restart_delay", cfg.restart_delay)) return false;

  return true;
}

std::vector<BridgeConfig> ParseBridges(const nlohmann::json& config,
                                       std::vector<std::string>& errors) {
  std::vector<BridgeConfig> bridges;
  if (!config.is_object()) return bridges;
  const auto list = config.find("bridges");
  if (list == config.end()) return bridges;
  if (!list->is_array()) {
    errors.emplace_back("'bridges' must be an array");
    return bridges;
  }
  for (const auto& entry : *list) {
    // value() would throw on an entry that isn't an object or on a name
    // that isn't a string: containerd crashed on such a containers.json.
    const auto name = entry.is_object() ? entry.find("name") : entry.end();
    if (name == entry.end() || !name->is_string() ||
        name->get_ref<const std::string&>().empty()) {
      errors.emplace_back(
          "bridge without a name: " +
          entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
      continue;
    }
    BridgeConfig bridge{.name = name->get<std::string>(), .address = {}};
    if (const auto address = entry.find("address"); address != entry.end()) {
      if (!address->is_string()) {
        errors.emplace_back("bridge '" + bridge.name +
                            "': 'address' must be a string");
        continue;
      }
      bridge.address = address->get<std::string>();
    }
    bridges.push_back(std::move(bridge));
  }
  return bridges;
}

const std::vector<ContainerConfig>& ContainersConfigParser::GetContainers()
    const {
  return containers_;
}

}  // namespace aember::containerd
