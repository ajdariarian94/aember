/**
 * @file status.cpp
 * @author Arian Ajdari
 * @brief StatusReporter implementation (see status.h).
 * @version 0.5
 * @date 2026-11-20
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/status.h>

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
// StatusReporter
// ---------------------------------------------------------------------------

Pid1Stats StatusReporter::ReadPid1Stats() {
  return Pid1Stats{
      .pid = 1,
      .vm_rss_kb = read_status_field(1, "VmRSS:"),
      .vm_size_kb = read_status_field(1, "VmSize:"),
      .threads = read_thread_count(1),
      .children = CountChildren(1),
      .uptime_seconds = ReadUptimeSeconds(),
  };
}

int StatusReporter::CountChildren(pid_t pid) {
  const auto path = std::format("/proc/{}/task/{}/children", pid, pid);
  if (std::ifstream f{path}; f.is_open()) {
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    return static_cast<int>(std::ranges::count(content, ' '));
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

long StatusReporter::ReadUptimeSeconds() {
  std::ifstream f{"/proc/uptime"};
  double uptime = 0.0;
  f >> uptime;
  return static_cast<long>(uptime);
}

nlohmann::json StatusReporter::Snapshot() const {
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

void StatusReporter::WriteFile(const nlohmann::json& payload,
                               std::string_view path) const {
  try {
    const std::string path_str{path};
    const std::string data = payload.dump();

    // std::ios::in | std::ios::out updates the EXISTING file in-place
    // without deleting/recreating the inode that LXC bound to the container!
    std::fstream f{path_str, std::ios::in | std::ios::out};
    if (!f.is_open()) { f.open(path_str, std::ios::out); }

    if (f.is_open()) {
      f.seekp(0);
      f << data;
      f.flush();
      f.close();

      std::error_code ec;
      std::filesystem::resize_file(path_str, data.size(), ec);
    }
  } catch (const std::exception& e) {
    log_.error("Writing {} failed: {}", path, e.what());
  }
}
}  // namespace aember::device_health
