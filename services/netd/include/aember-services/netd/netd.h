/**
 * @file netd.h
 * @author Arian Ajdari
 * @brief aember-netd: the network service (services/netd).
 *
 *        Listed in services.json with "ipc": true, so PID 1 starts it with
 *        a channel (fd 3). It brings up the interfaces from network.json
 *        (keeping addresses
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

namespace aember::netd {

struct NetdOptions {
  std::string network_json = "/etc/aember/network.json";
  /// Everything netd logs (also while a debug shell mutes the console).
  std::string log_file = "/var/log/aember-netd.log";
};

/// Run the netd role; returns the process exit code.
int RunNetd(const NetdOptions& options = {});

}  // namespace aember::netd
