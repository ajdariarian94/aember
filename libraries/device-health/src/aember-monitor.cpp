/**
 * @file aember-monitor.cpp
 * @author Arian Ajdari
 * @brief AemberMonitor + AemberMonitorClient implementation.
 * @version 0.4
 * @date 2026-06-30
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/aember-monitor.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace aember::device_health {

// ---------------------------------------------------------------------------
// /proc helpers
// ---------------------------------------------------------------------------

namespace {

long read_status_field(pid_t pid, std::string_view field) {
  std::ifstream f{std::format("/proc/{}/status", pid)};
  std::string line;
  while (std::getline(f, line)) {
    if (line.starts_with(field)) {
      long v = 0;
      std::istringstream ss{line.substr(line.find(':') + 1)};
      ss >> v;
      return v;
    }
  }
  return 0;
}

int read_thread_count(pid_t pid) {
  std::ifstream f{std::format("/proc/{}/status", pid)};
  std::string line;
  while (std::getline(f, line)) {
    if (line.starts_with("Threads:")) {
      int v = 0;
      std::istringstream ss{line.substr(line.find(':') + 1)};
      ss >> v;
      return v;
    }
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// AemberMonitor
// ---------------------------------------------------------------------------

Pid1Stats AemberMonitor::ReadPid1Stats() {
  return Pid1Stats{
      .pid = 1,
      .vm_rss_kb = read_status_field(1, "VmRSS:"),
      .vm_size_kb = read_status_field(1, "VmSize:"),
      .threads = read_thread_count(1),
      .children = CountChildren(1),
      .uptime_seconds = ReadUptimeSeconds(),
  };
}

int AemberMonitor::CountChildren(pid_t pid) {
  const auto path = std::format("/proc/{}/task/{}/children", pid, pid);
  if (std::ifstream f{path}; f.is_open()) {
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    return static_cast<int>(std::ranges::count(content, ' ') + 1);
  }

  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator{
           "/proc",
           std::filesystem::directory_options::skip_permission_denied}) {
    if (!entry.is_directory()) continue;
    const auto name = entry.path().filename().string();
    if (!std::ranges::all_of(name, ::isdigit)) continue;

    std::ifstream status{std::format("/proc/{}/status", name)};
    std::string line;
    while (std::getline(status, line)) {
      if (line.starts_with("PPid:")) {
        pid_t ppid = 0;
        std::istringstream ss{line.substr(line.find(':') + 1)};
        ss >> ppid;
        if (ppid == pid) ++count;
        break;
      }
    }
  }
  return count;
}

long AemberMonitor::ReadUptimeSeconds() {
  std::ifstream f{"/proc/uptime"};
  double uptime = 0.0;
  f >> uptime;
  return static_cast<long>(uptime);
}

nlohmann::json AemberMonitor::Snapshot() const {
  auto stats = ReadPid1Stats();
  stats.session_uptime = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start_time_)
                             .count();

  return {{"pid1",
           {
               {"pid", stats.pid},
               {"vm_rss_kb", stats.vm_rss_kb},
               {"vm_size_kb", stats.vm_size_kb},
               {"threads", stats.threads},
               {"children", stats.children},
               {"uptime_seconds", stats.uptime_seconds},
               {"session_uptime", stats.session_uptime},
           }}};
}

void AemberMonitor::Log(const nlohmann::json& payload) const {
  const auto& pid1 = payload["pid1"];

  log_.info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  log_.info(
      "  Aember PID1  │ pid={} │ rss={}kB │ threads={} │ children={} │ "
      "uptime={}s",
      pid1["pid"].get<int>(),
      pid1["vm_rss_kb"].get<long>(),
      pid1["threads"].get<int>(),
      pid1["children"].get<int>(),
      pid1["uptime_seconds"].get<long>());

  if (payload.contains("network")) {
    const auto& net = payload["network"];
    log_.info("  Network      │ {} │ iface={} │ rtt={}ms │ ip={}",
              net.value("online", false) ? "ONLINE " : "OFFLINE",
              net.value("interface", ""),
              net.value("rtt_ms", 0),
              net.value("ip", ""));
  }

  if (payload.contains("services")) {
    const auto& services = payload["services"];
    log_.info("  Services     │ {} service(s)", services.size());
    for (const auto& svc : services) {
      log_.info("    ├─ {} [{}]",
                svc["name"].get<std::string>(),
                svc["state"].get<std::string>());
    }
  }

  log_.info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ---------------------------------------------------------------------------
// AemberMonitorClient
// ---------------------------------------------------------------------------

AemberMonitorClient::AemberMonitorClient(std::string_view service_name)
    : service_name_(service_name), log_(std::string{"aember-monitor-client"}) {}

AemberMonitorClient::SelfStats AemberMonitorClient::ReadSelfStats() {
  const pid_t pid = getpid();
  return SelfStats{
      .pid = static_cast<int>(pid),
      .ppid = static_cast<int>(getppid()),
      .vm_rss_kb = read_status_field(pid, "VmRSS:"),
  };
}

long AemberMonitorClient::ReadPid1RssKb() {
  return read_status_field(1, "VmRSS:");
}

nlohmann::json AemberMonitorClient::Snapshot() const {
  auto self = ReadSelfStats();
  self.uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - start_time_)
                      .count();
  self.pid1_rss_kb = ReadPid1RssKb();

  return {
      {"service", service_name_},
      {"pid", self.pid},
      {"ppid", self.ppid},
      {"vm_rss_kb", self.vm_rss_kb},
      {"uptime_s", self.uptime_s},
      {"pid1_rss_kb", self.pid1_rss_kb},
  };
}

void AemberMonitorClient::Report() {
  const auto s = Snapshot();
  log_.info(
      "[{}] pid={} ppid={} (aember PID1) │ rss={}kB │ uptime={}s │ "
      "pid1_rss={}kB",
      s["service"].get<std::string>(),
      s["pid"].get<int>(),
      s["ppid"].get<int>(),
      s["vm_rss_kb"].get<long>(),
      s["uptime_s"].get<long>(),
      s["pid1_rss_kb"].get<long>());
}

}  // namespace aember::device_health
