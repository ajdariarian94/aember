/**
 * @file container-config.h
 * @author Arian Ajdari
 * @brief One container from containers.json.
 * @version 0.2
 * @date 2026-11-18
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aember::containerd {

/// When containerd starts a container again after its init exited.
enum class RestartPolicy {
  kNo,         ///< never (default)
  kOnFailure,  ///< after an unexpected exit
  kAlways,     ///< after any exit containerd didn't ask for
};

inline std::string_view ToString(RestartPolicy p) {
  switch (p) {
    case RestartPolicy::kNo:
      return "no";
    case RestartPolicy::kOnFailure:
      return "on-failure";
    case RestartPolicy::kAlways:
      return "always";
  }
  return "no";
}

inline std::optional<RestartPolicy> RestartPolicyFromString(
    std::string_view s) {
  if (s == "no" || s == "never") return RestartPolicy::kNo;
  if (s == "on-failure") return RestartPolicy::kOnFailure;
  if (s == "always") return RestartPolicy::kAlways;
  return std::nullopt;
}

struct ContainerConfig {
  std::string name;
  std::string squashfs;
  std::string rootfs;
  std::string config_path;
  std::vector<std::string> args;

  /// Run the args under a small init (the host's static BusyBox, bind
  /// mounted as /busybox.aember-init) that passes SIGTERM/INT/HUP on: an app
  /// that is the container's init never gets SIGTERM (the kernel drops
  /// signals without a handler for a namespace init), so it can't stop
  /// gracefully. "init": false runs the args as init directly.
  bool init{true};
  /// How long a graceful stop may take before the container is killed.
  std::chrono::seconds stop_timeout{10};

  RestartPolicy restart_policy{RestartPolicy::kNo};
  /// Restarts in a row before giving up; 0 = no limit. A container that
  /// ran for kStableRun counts as healthy again (the count resets).
  unsigned max_restart_attempts{0};
  std::chrono::seconds restart_delay{5};
};

/// A run this long resets the restart count.
inline constexpr std::chrono::seconds kStableRun{60};

}  // namespace aember::containerd
