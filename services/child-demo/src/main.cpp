/**
 * @file main.cpp
 * @brief aember-child-demo: the smallest supervised service. It logs a line
 *        every few seconds and exits cleanly on SIGTERM/SIGINT, so PID 1's
 *        restart and stop handling can be watched.
 */
#include <aember-libs/utils/logging/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>

#include <unistd.h>

static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::atomic<bool> g_running{true};

static void handle_signal(int) {
  g_running = false;
  g_cv.notify_all();
}

int main() {
  // Exit cleanly on SIGTERM/SIGINT so PID 1 sees SIGCHLD at once.
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);

  aember::utils::logging::Logger log_{"aember-child-demo"};
  aember::utils::logging::enable_console_silence();
  aember::utils::logging::enable_file_logging("/var/log/aember-child-demo.log");

  log_.info(
      "aember-child-demo started (pid {}, parent {})", ::getpid(), ::getppid());
  long ticks = 0;
  while (g_running) {
    std::unique_lock lock{g_mtx};
    g_cv.wait_for(lock, std::chrono::seconds(10));
    if (g_running) log_.info("alive ({} s)", (++ticks) * 10);
  }
  log_.info("aember-child-demo shutting down gracefully");
  return 0;
}
