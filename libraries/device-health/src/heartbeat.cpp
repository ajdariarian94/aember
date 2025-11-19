/**
 * @file heartbeat.cpp
 * @author Arian Ajdari
 * @brief Library implementation for Heartbeat
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#include <aember-libs/device-health/heartbeat.h>  // for aember::device_health::Heartbeat

#include <spdlog/spdlog.h>    // for spdlog::error, spdlog::info
#include <nlohmann/json.hpp>  // for nlohmann::json

namespace aember::device_health {

Heartbeat::Heartbeat(const std::function<void(const nlohmann::json&)>& callback,
                     const std::chrono::milliseconds& interval)
    : running_(false), interval_(interval), callback_(callback), log_("heartbeat") {}

Heartbeat::~Heartbeat() {
  Stop();
}

void Heartbeat::Start() {
  log_.info("Starting heartbeat");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) { return; }
    running_ = true;
  }

  check_thread_ = std::thread(&Heartbeat::Run, this);
}

void Heartbeat::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }

  cv_.notify_all();
  if (check_thread_.joinable()) { check_thread_.join(); }
}

void Heartbeat::Run() {
  std::unique_lock<std::mutex> lock(mutex_);

  while (running_) {
    Beep();

    cv_.wait_for(lock, interval_, [this] { return !running_; });
  }
}

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
