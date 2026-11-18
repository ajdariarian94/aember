/**
 * @file main.cpp
 * @author Arian Ajdari
 * @brief aember-netd: the network service. Started by PID 1 from
 *        services.json ("ipc": true); `aember-netd [network.json]`.
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/netd/netd.h>

int main(int argc, char** argv) {
  aember::netd::NetdOptions options;
  if (argc > 1) options.network_json = argv[1];
  return aember::netd::RunNetd(options);
}
