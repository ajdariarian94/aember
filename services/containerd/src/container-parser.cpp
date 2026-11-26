/**
 * @file container-parser.cpp
 * @author Arian Ajdari
 * @brief containers.json and a container's config.json (see
 *        container-parser.h).
 * @version 0.3
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/package.h>
#include <aember-services/containerd/container-parser.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>

namespace aember::containerd {

namespace {

/// A whole number that fits an unsigned. get<unsigned>() alone wraps bigger
/// ones: "max_restart_attempts": 4294967296 would be 0 (= unlimited).
bool FitsUnsigned(const nlohmann::json& value) {
  return value.is_number_unsigned() &&
         value.get<std::uint64_t>() <= std::numeric_limits<unsigned>::max();
}

std::string Dump(const nlohmann::json& j) {
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::expected<nlohmann::json, std::string> ReadJson(const std::string& path) {
  std::ifstream file{path};
  if (!file) return std::unexpected(path + ": cannot be read");
  std::stringstream text;
  text << file.rdbuf();
  auto json = nlohmann::json::parse(text.str(), nullptr, false);
  if (json.is_discarded()) return std::unexpected(path + ": not JSON");
  return json;
}

}  // namespace

std::expected<BoardContainers, std::string> ParseBoardContainers(
    const nlohmann::json& json) {
  if (!json.is_object()) return std::unexpected("not a JSON object");
  BoardContainers board;
  if (const auto it = json.find("allow_unsigned"); it != json.end()) {
    if (!it->is_boolean()) {
      return std::unexpected("'allow_unsigned' must be true or false");
    }
    board.allow_unsigned = it->get<bool>();
  }
  const auto list = json.find("containers");
  if (list == json.end()) return board;  // no containers
  if (!list->is_array()) return std::unexpected("'containers' must be a list");
  for (const auto& entry : *list) {
    if (!entry.is_object()) {
      return std::unexpected("a container entry isn't an object: " +
                             Dump(entry));
    }
    if (entry.contains("container")) {
      return std::unexpected(
          "containers.json in the format before .aember packages (entry " +
          Dump(entry.value("name", nlohmann::json{})) +
          " has \"container\"): list only names and dependencies, the rest "
          "is in the package (doc/aember-format.md)");
    }
    const auto name = entry.find("name");
    if (name == entry.end() || !name->is_string() ||
        !utils::package::ValidName(name->get_ref<const std::string&>())) {
      return std::unexpected("a container entry without a valid name: " +
                             Dump(entry));
    }
    StartGraph::Node node{.name = name->get<std::string>(), .dependencies = {}};
    if (const auto deps = entry.find("dependencies"); deps != entry.end()) {
      if (!deps->is_array()) {
        return std::unexpected("'" + node.name +
                               "': 'dependencies' must be a list");
      }
      for (const auto& d : *deps) {
        if (!d.is_string()) {
          return std::unexpected("'" + node.name +
                                 "': dependencies must be names");
        }
        node.dependencies.push_back(d.get<std::string>());
      }
    }
    board.containers.push_back(std::move(node));
  }
  return board;
}

std::expected<BoardContainers, std::string> LoadBoardContainers(
    const std::string& path) {
  auto json = ReadJson(path);
  if (!json) return std::unexpected(json.error());
  auto board = ParseBoardContainers(*json);
  if (!board) return std::unexpected(path + ": " + board.error());
  return board;
}

std::expected<ContainerConfig, std::string> ParseContainerConfig(
    const nlohmann::json& json, std::string_view root) {
  if (!json.is_object()) return std::unexpected("not a JSON object");
  const auto name = json.find("name");
  if (name == json.end() || !name->is_string() ||
      !utils::package::ValidName(name->get_ref<const std::string&>())) {
    return std::unexpected("no valid 'name'");
  }
  ContainerConfig cfg;
  cfg.name = name->get<std::string>();
  const std::string dir = std::string{root} + "/" + cfg.name;
  cfg.squashfs = dir + "/" + std::string{utils::package::kImageFile};
  cfg.config_path = dir + "/" + std::string{utils::package::kLxcFile};
  cfg.rootfs = std::string{root} + "/containers/" + cfg.name;

  const auto command = json.find("command");
  if (command == json.end() || !command->is_array() || command->empty()) {
    return std::unexpected("'command' must be a non-empty list");
  }
  for (const auto& arg : *command) {
    if (!arg.is_string()) {
      return std::unexpected("'command' must be a list of strings");
    }
    cfg.args.push_back(arg.get<std::string>());
  }

  std::string error;
  const auto seconds = [&](const char* key, std::chrono::seconds& out) {
    const auto it = json.find(key);
    if (it == json.end()) return true;
    if (!FitsUnsigned(*it)) {
      error = std::string{"'"} + key + "' must be a number of seconds";
      return false;
    }
    out = std::chrono::seconds{it->get<unsigned>()};
    return true;
  };
  if (const auto it = json.find("init"); it != json.end()) {
    if (!it->is_boolean())
      return std::unexpected("'init' must be true or false");
    cfg.init = it->get<bool>();
  }
  if (!seconds("stop_timeout", cfg.stop_timeout)) {
    return std::unexpected(error);
  }
  if (const auto it = json.find("restart_policy"); it != json.end()) {
    const auto policy = it->is_string()
                            ? RestartPolicyFromString(it->get<std::string>())
                            : std::nullopt;
    if (!policy) {
      return std::unexpected(
          "'restart_policy' must be \"no\", \"on-failure\" or \"always\"");
    }
    cfg.restart_policy = *policy;
  }
  if (const auto it = json.find("max_restart_attempts"); it != json.end()) {
    if (!FitsUnsigned(*it)) {
      return std::unexpected("'max_restart_attempts' must be >= 0");
    }
    cfg.max_restart_attempts = it->get<unsigned>();
  }
  if (!seconds("restart_delay", cfg.restart_delay)) {
    return std::unexpected(error);
  }
  return cfg;
}

std::expected<ContainerConfig, std::string> LoadContainerConfig(
    std::string_view name, std::string_view root) {
  const auto path = std::string{root} + "/" + std::string{name} + "/" +
                    std::string{utils::package::kConfigFile};
  auto json = ReadJson(path);
  if (!json) return std::unexpected(json.error() + " (not installed?)");
  auto cfg = ParseContainerConfig(*json, root);
  if (!cfg) return std::unexpected(path + ": " + cfg.error());
  if (cfg->name != name) {
    return std::unexpected(path + ": names '" + cfg->name + "'");
  }
  return cfg;
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
      errors.emplace_back("bridge without a name: " + Dump(entry));
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

}  // namespace aember::containerd
