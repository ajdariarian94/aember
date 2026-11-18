/**
 * @file container-entry.h
 * @author Arian Ajdari
 * @brief A registered container and its runtime state.
 * @version 0.2
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-services/containerd/container-config.h>
#include <aember-services/containerd/container-state.h>

#include <chrono>

struct lxc_container;

namespace aember::containerd {

struct ContainerEntry {
  ContainerConfig config;
  ContainerState state{ContainerState::kStopped};
  lxc_container* lxc{nullptr};
  /// Runs under the BusyBox init wrapper (it is bind mounted).
  bool init_wrapper{false};
  /// containerd asked it to stop: never restarted.
  bool stop_requested{false};
  /// Restarts in a row (reset after kStableRun).
  unsigned restarts{0};
  std::chrono::steady_clock::time_point started_at{};
};

}  // namespace aember::containerd
