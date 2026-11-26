/**
 * @file container-parser.h
 * @author Arian Ajdari
 * @brief containers.json (a board's: which containers, their dependencies,
 *        the bridges) and a container's own config.json (in its .aember
 *        package: how to start it).
 *
 *   /etc/aember/containers.json
 *     {"allow_unsigned": false,
 *      "bridges": [{"name": "lxcbr0", "address": "10.0.3.1/24"}],
 *      "containers": [{"name": "aember-stats"},
 *                     {"name": "aember-monitor",
 *                      "dependencies": ["aember-stats"]}]}
 *
 *   /var/lib/aember/<name>/config.json   (doc/aember-format.md)
 *     {"name": "aember-monitor", "command": ["/usr/bin/aember-monitor"],
 *      "init": true, "stop_timeout": 5, "restart_policy": "always",
 *      "max_restart_attempts": 0, "restart_delay": 2}
 *
 * @version 0.3
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-services/containerd/container-config.h>
#include <aember-services/containerd/start-graph.h>

#include <nlohmann/json.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace aember::containerd {

/// Where packages are installed (one directory per container) and where
/// the containers' root file systems are mounted.
inline constexpr std::string_view kPackagesRoot = "/var/lib/aember";

/// One entry of containers.json's "bridges" list.
struct BridgeConfig {
  std::string name;
  std::string address;  ///< CIDR, e.g. "10.0.3.1/24"; empty: no address
};

/// The "bridges" list of a parsed containers.json. An entry that isn't
/// {"name": "<non-empty>", "address": "<optional>"} is skipped and described
/// in @p errors; a missing list is no bridges. Never throws.
std::vector<BridgeConfig> ParseBridges(const nlohmann::json& config,
                                       std::vector<std::string>& errors);

/// A board's containers.json without the bridges.
struct BoardContainers {
  /// Accept packages without a signature (development boards only).
  bool allow_unsigned{false};
  std::vector<StartGraph::Node> containers;
};

/// containers.json; an error for anything malformed (nothing starts then:
/// a half-read list could start a container without what it depends on).
/// Never throws.
std::expected<BoardContainers, std::string> ParseBoardContainers(
    const nlohmann::json& json);
std::expected<BoardContainers, std::string> LoadBoardContainers(
    const std::string& path);

/// A container's config.json; the paths come from its name under @p root
/// (<root>/<name>/image.squashfs, lxc.container.conf;
/// <root>/containers/<name> for its root file system). Never throws.
std::expected<ContainerConfig, std::string> ParseContainerConfig(
    const nlohmann::json& json, std::string_view root = kPackagesRoot);
/// <root>/<name>/config.json, which must name @p name.
std::expected<ContainerConfig, std::string> LoadContainerConfig(
    std::string_view name, std::string_view root = kPackagesRoot);

}  // namespace aember::containerd
