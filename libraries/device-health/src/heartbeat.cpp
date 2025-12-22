/**
 * @file heartbeat.cpp
 * @author Arian Ajdari
 * @brief Library implementation for Heartbeat service
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/heartbeat.h>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace aember::device_health {

Heartbeat::Heartbeat(const std::function<void(const nlohmann::json&)>& callback,
                     const std::chrono::milliseconds& interval)
    : running_(false),
      interval_(interval),
      callback_(callback),
      log_("heartbeat") {}

/**
 * @brief Destructor stops the heartbeat thread if still running.
 */
Heartbeat::~Heartbeat() {
  Stop();
}

/**
 * @brief Starts the heartbeat background thread.
 * If already running, does nothing.
 */
void Heartbeat::Start() {
  log_.info("Starting heartbeat");

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) { return; }
    running_ = true;
  }

  // Launch background thread
  check_thread_ = std::thread(&Heartbeat::Run, this);
}

/**
 * @brief Stops the heartbeat background thread and waits for it to exit.
 */
void Heartbeat::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }

  cv_.notify_all();

  if (check_thread_.joinable()) { check_thread_.join(); }
}

/**
 * @brief Internal function run in the background thread.
 * Periodically calls Beep() at the specified interval.
 */
void Heartbeat::Run() {
  std::unique_lock<std::mutex> lock(mutex_);

  while (running_) {
    Beep();

    // Wait for next interval or stop signal
    cv_.wait_for(lock, interval_, [this] { return !running_; });
  }
}

/**
 * @brief Sends a heartbeat by invoking the callback with JSON payload.
 * The payload contains:
 * - "status": always "online"
 * - "timestamp": milliseconds since epoch
 */
void Heartbeat::Beep() {
  try {
    nlohmann::json heartbeat_data;
    heartbeat_data["status"] = "online";
    heartbeat_data["timestamp"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();

    callback_(heartbeat_data);
  } catch (const std::exception& e) {
    log_.error("Error in Beep: {}", e.what());
  }
}

}  // namespace aember::device_health
