/**
 * @file containerd.h
 * @author Arian Ajdari
 * @brief aember-containerd: the container service (services/containerd).
 *
 *        Listed in services.json with "ipc": true, so PID 1 starts it with
 *        a channel (fd 3). It creates the container bridges ("bridges" in
 *        containers.json), loads liblxc (dlopen), starts the containers
 *        from containers.json (or
 *        adopts the ones a previous containerd left running), reports
 *        "ready" and every container state change, and stops all
 *        containers on SIGTERM. It follows each container's init through
 *        a pidfd. It is deliberately not a subreaper: the LXC monitors have
 *        a parent-death signal, so they must stay PID 1's orphans or a
 *        containerd crash would kill all containers.
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::containerd {

struct ContainerdOptions {
  std::string containers_json = "/etc/aember/containers.json";
};

/// Bridges from the "bridges" list of @p containers_json (name, address
/// CIDR): created if missing, addressed, up. Returns how many failed.
int EnsureBridges(const std::string& containers_json);

/// Run containerd; returns the process exit code.
int RunContainerd(const ContainerdOptions& options = {});

}  // namespace aember::containerd
