/**
 * @file netd.h
 * @author Arian Ajdari
 * @brief `aember netd`: the network, out of PID 1.
 *
 *        A built-in service PID 1 starts with a channel (fd 3). It brings
 *        up interfaces and bridges from network.json (keeping addresses
 *        that exist), reports "ready" and the connectivity status (on
 *        change, and every 10 s), and exits on SIGTERM leaving the kernel's
 *        network configuration as it is, so a restart doesn't drop it.
 *
 * @version 0.1
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <string>

namespace aember::network {

struct NetdOptions {
  std::string network_json = "/etc/aember/network.json";
};

/// Run the netd role; returns the process exit code.
int RunNetd(const NetdOptions& options = {});

}  // namespace aember::network
