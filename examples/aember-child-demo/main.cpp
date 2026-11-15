#include <aember-libs/device-health/aember-monitor.h>
#include <aember-libs/utils/logging/logging.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <string_view>

// ---------------------------------------------------------------------------
// Monitored fields of AemberMonitorClient::SelfStats, logged at startup.
// Keep in sync with the SelfStats definition in aember-monitor.h.
// ---------------------------------------------------------------------------

constexpr std::string_view kMonitorClientName = "AemberMonitorClient";

constexpr std::array<std::string_view, 5> kSelfStatsFields{
    "pid", "ppid", "vm_rss_kb", "uptime_s", "pid1_rss_kb"};

// ---------------------------------------------------------------------------
// Graceful shutdown via SIGTERM / SIGINT
// ---------------------------------------------------------------------------

static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::atomic<bool> g_running{true};

static void handle_signal(int) {
  g_running = false;
  g_cv.notify_all();
}

int main() {
  // Install signal handlers — process exits cleanly on SIGTERM/SIGINT
  // so PID1 receives SIGCHLD immediately and can restart us.
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);

  aember::utils::logging::Logger log_{"aember-child-demo"};

  aember::utils::logging::enable_console_silence();
  aember::utils::logging::enable_file_logging("/var/log/aember-child-demo.log");

  aember::device_health::AemberMonitorClient monitor{"aember-child-demo"};

  log_.info("Monitor type: {}", kMonitorClientName);

  log_.info("Monitored fields:");
  for (std::size_t i = 0; i < kSelfStatsFields.size(); ++i) {
    log_.info("  {} {}",
              i + 1 < kSelfStatsFields.size() ? "├─" : "└─",
              kSelfStatsFields[i]);
  }

  monitor.Report();

  while (g_running) {
    std::unique_lock lock{g_mtx};
    g_cv.wait_for(lock, std::chrono::seconds(1));
    if (g_running) { monitor.Report(); }
  }

  log_.info("aember-child-demo shutting down gracefully");
  return 0;
}
