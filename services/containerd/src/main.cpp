/**
 * @file main.cpp
 * @author Arian Ajdari
 * @brief aember-containerd: the container service. Started by PID 1 from
 *        services.json ("ipc": true); `aember-containerd [containers.json]`.
 * @version 0.1
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/containerd.h>

int main(int argc, char** argv) {
  aember::containerd::ContainerdOptions options;
  if (argc > 1) options.containers_json = argv[1];
  return aember::containerd::RunContainerd(options);
}
