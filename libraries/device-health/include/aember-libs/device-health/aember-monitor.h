/**
 * @file aember-monitor.h
 * @author Arian Ajdari
 * @brief AemberMonitor — collects runtime statistics for the Aember init
 *        system and its supervised children.
 * @version 0.4
 * @date 2026-06-30
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace aember::device_health {

// ---------------------------------------------------------------------------
// Stats structs
// ---------------------------------------------------------------------------

struct Pid1Stats {
  int pid{1};
  long vm_rss_kb{0};
  long vm_size_kb{0};
  int threads{0};
  int children{0};
  long uptime_seconds{0};
  long session_uptime{0};
};

struct SelfStats {
  int pid{0};
  int ppid{0};
  long vm_rss_kb{0};
  long uptime_s{0};
  long pid1_rss_kb{0};
};

// ---------------------------------------------------------------------------
// AemberMonitor
//
// Reads PID1 and system stats from /proc. No external dependencies.
// Usable from any executable — not just aember-init.
//
// In aember-init the Heartbeat callback enriches the snapshot:
//
//   auto monitor = std::make_unique<AemberMonitor>();
//   heartbeat_ = std::make_unique<Heartbeat>(
//       [&m = *monitor, &net = *network_manager_, &svc = *service_manager_]
//       (const nlohmann::json& base) {
//           auto payload = m.Snapshot();
//           payload["network"]  = { ... };
//           payload["services"] = { ... };
//           payload.merge_patch(base);
//           m.Log(payload);
//       });
// ---------------------------------------------------------------------------

class AemberMonitor {
 public:
  using Logger = aember::utils::logging::Logger;

  AemberMonitor() = default;

  /// Collect current PID1 + system stats as JSON.
  [[nodiscard]] nlohmann::json Snapshot() const;

  /// Log a pre-built payload — call after enriching with network/services.
  void Log(const nlohmann::json& payload) const;

  /// Read raw stats struct.
  [[nodiscard]] static Pid1Stats ReadPid1Stats();

 private:
  static int CountChildren(pid_t pid);
  static long ReadUptimeSeconds();

  std::chrono::steady_clock::time_point start_time_{
      std::chrono::steady_clock::now()};

  mutable Logger log_{"aember-monitor"};
};

// ---------------------------------------------------------------------------
// AemberMonitorClient
//
// Lightweight version for child processes (echo-aember etc.).
// Reports own identity + PID1 RSS — no other dependencies.
// ---------------------------------------------------------------------------

class AemberMonitorClient {
 public:
  using Logger = aember::utils::logging::Logger;

  struct SelfStats {
    int pid{0};
    int ppid{0};
    long vm_rss_kb{0};
    long uptime_s{0};
    long pid1_rss_kb{0};
  };

  explicit AemberMonitorClient(std::string_view service_name);

  /// Log one line of self + PID1 stats.
  void Report();

  /// Return stats as JSON.
  [[nodiscard]] nlohmann::json Snapshot() const;

 private:
  static SelfStats ReadSelfStats();
  static long ReadPid1RssKb();

  std::string service_name_;
  std::chrono::steady_clock::time_point start_time_{
      std::chrono::steady_clock::now()};
  mutable Logger log_;
};

}  // namespace aember::device_health
