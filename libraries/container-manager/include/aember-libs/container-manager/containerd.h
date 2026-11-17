/**
 * @file containerd.h
 * @author Arian Ajdari
 * @brief `aember containerd`: the containers, out of PID 1.
 *
 *        A built-in service PID 1 starts with a channel (fd 3). It loads
 *        liblxc (dlopen), starts the containers from containers.json (or
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

namespace aember::container_manager {

struct ContainerdOptions {
  std::string containers_json = "/etc/aember/containers.json";
};

/// Run the containerd role; returns the process exit code.
int RunContainerd(const ContainerdOptions& options = {});

}  // namespace aember::container_manager
