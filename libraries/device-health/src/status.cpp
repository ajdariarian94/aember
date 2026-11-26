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

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace aember::device_health {

// ---------------------------------------------------------------------------
// /proc helpers
// ---------------------------------------------------------------------------

namespace {

long read_status_field(pid_t pid, std::string_view field) {
  std::ifstream f{fmt::format("/proc/{}/status", pid)};
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
  std::ifstream f{fmt::format("/proc/{}/status", pid)};
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

int StatusReporter::CountChildren(pid_t pid,
                                  const std::filesystem::path& proc_root) {
  const auto path = proc_root / fmt::format("{}/task/{}/children", pid, pid);
  if (std::ifstream f{path}; f.is_open()) {
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    return static_cast<int>(std::ranges::count(content, ' '));
  }
  return CountChildrenByScan(pid, proc_root);
}

int StatusReporter::CountChildrenByScan(
    pid_t pid, const std::filesystem::path& proc_root) {
  // Plain syscalls and one stack buffer per entry (see
  // ChildSupervisor::ListZombieChildren): the stat line, not the much
  // longer status file.
  int count = 0;
  DIR* dir = ::opendir(proc_root.c_str());
  if (dir == nullptr) return 0;
  const int dir_fd = ::dirfd(dir);
  std::array<char, 512> buf{};
  while (const dirent* entry = ::readdir(dir)) {
    const std::string_view name{entry->d_name};
    if (name.empty() || !std::ranges::all_of(name, ::isdigit)) continue;
    const std::string stat_path = std::string{name} + "/stat";
    const int fd = ::openat(dir_fd, stat_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    const ssize_t n = ::read(fd, buf.data(), buf.size() - 1);
    ::close(fd);
    if (n <= 0) continue;
    // "<pid> (<comm>) <state> <ppid> ...": comm may hold ')' and spaces.
    const std::string_view line{buf.data(), static_cast<std::size_t>(n)};
    const auto close = line.rfind(')');
    if (close == std::string_view::npos || close + 4 >= line.size()) continue;
    pid_t ppid = 0;
    std::from_chars(line.data() + close + 4, line.data() + line.size(), ppid);
    if (ppid == pid) ++count;
  }
  ::closedir(dir);
  return count;
}

long ParseUptimeSeconds(std::string_view proc_uptime) {
  const auto* begin = proc_uptime.data();
  const auto* end = begin + proc_uptime.size();
  while (begin != end && (*begin == ' ' || *begin == '\t')) ++begin;
  double uptime = 0.0;
  if (std::from_chars(begin, end, uptime).ec != std::errc{}) return 0;
  // Casting inf, nan or anything beyond long to long is undefined.
  if (!std::isfinite(uptime) || uptime < 0) return 0;
  constexpr auto kMax = static_cast<double>(std::numeric_limits<long>::max());
  return uptime >= kMax ? std::numeric_limits<long>::max()
                        : static_cast<long>(uptime);
}

long StatusReporter::ReadUptimeSeconds() {
  std::ifstream f{"/proc/uptime"};
  std::string line;
  std::getline(f, line);
  return ParseUptimeSeconds(line);
}

nlohmann::json StatusReporter::Snapshot() const {
  auto stats = Pid1Stats{
      .pid = 1,
      .vm_rss_kb = read_status_field(1, "VmRSS:"),
      .vm_size_kb = read_status_field(1, "VmSize:"),
      .threads = read_thread_count(1),
      .children = 0,
      .uptime_seconds = ReadUptimeSeconds(),
  };
  const auto now = std::chrono::steady_clock::now();
  if (children_at_ == std::chrono::steady_clock::time_point{} ||
      now - children_at_ >= kChildrenMaxAge) {
    children_ = CountChildren(1);
    children_at_ = now;
  }
  stats.children = children_;
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
    // Bytes that aren't UTF-8 become U+FFFD: a throwing dump() would leave
    // the file stale for good.
    const std::string data =
        payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

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
