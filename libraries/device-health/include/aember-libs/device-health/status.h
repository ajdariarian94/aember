/**
 * @file status.h
 * @author Arian Ajdari
 * @brief StatusReporter: PID 1's status snapshot and the status file.
 * @version 0.5
 * @date 2026-11-20
 *
 * aember's PID 1 writes its status (its own /proc stats, merged by
 * aember-init with the network state and every service's and container's
 * state) to /var/log/aember-status.json once per heartbeat. Containers see
 * it at /host-logs/aember-status.json; the aember-stats container serves it
 * to viewers such as aember-monitor. PID 1 itself prints nothing per tick.
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string_view>

#include <sys/types.h>

namespace aember::device_health {

/// Where PID 1 writes its status (aember-init passes its log dir).
inline constexpr std::string_view kStatusFile = "/var/log/aember-status.json";

/// Seconds of uptime from /proc/uptime's text ("12345.67 54321.00"); 0 if
/// it can't be read, clamped to long. Never undefined (inf, nan, huge).
long ParseUptimeSeconds(std::string_view proc_uptime);

struct Pid1Stats {
  int pid{1};
  long vm_rss_kb{0};
  long vm_size_kb{0};
  int threads{0};
  int children{0};
  long uptime_seconds{0};
  long session_uptime{0};
};

/**
 * Reads PID 1's stats from /proc and writes the status file.
 *
 *   StatusReporter status;
 *   auto payload = status.Snapshot();      // {"pid1": {...}}
 *   payload["network"]  = ...;             // aember-init adds these
 *   payload["services"] = ...;
 *   status.WriteFile(payload, dir + "/aember-status.json");
 */
class StatusReporter {
 public:
  using Logger = aember::utils::logging::Logger;

  StatusReporter() = default;

  /// PID 1's current stats as {"pid1": {...}}.
  [[nodiscard]] nlohmann::json Snapshot() const;

  /// Rewrites @p path in place with @p payload (one line of JSON). In place:
  /// the inode stays, so a reader that has the file open, or a bind mount of
  /// the file itself, keeps seeing it. Errors are logged, never thrown.
  void WriteFile(const nlohmann::json& payload,
                 std::string_view path = kStatusFile) const;

  [[nodiscard]] static Pid1Stats ReadPid1Stats();

  /// Children of @p pid: /proc/<pid>/task/<pid>/children, or a scan of
  /// every /proc/<n>/status PPid: line where that file doesn't exist
  /// (kernels without CONFIG_PROC_CHILDREN).
  static int CountChildren(pid_t pid);
  static int CountChildrenByScan(pid_t pid);

 private:
  static long ReadUptimeSeconds();

  std::chrono::steady_clock::time_point start_time_{
      std::chrono::steady_clock::now()};

  mutable Logger log_{"status"};
};

}  // namespace aember::device_health
