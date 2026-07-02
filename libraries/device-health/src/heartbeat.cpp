/**
 * @file heartbeat.cpp
 * @author Arian Ajdari
 * @brief Heartbeat service implementation.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/device-health/heartbeat.h>

#include <nlohmann/json.hpp>

namespace aember::device_health {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

Heartbeat::Heartbeat(Callback callback, std::chrono::milliseconds interval)
    : interval_(interval), callback_(std::move(callback)) {}

Heartbeat::~Heartbeat() {
  Stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Heartbeat::Start() {
  if (thread_.joinable()) return;  // already running

  log_.info("Starting heartbeat (interval={}ms)", interval_.count());

  // jthread passes a stop_token as the first argument automatically.
  thread_ = std::jthread{[this](std::stop_token st) { Run(std::move(st)); }};
}

void Heartbeat::Stop() {
  // request_stop() signals the stop_token; jthread joins in its destructor.
  thread_.request_stop();
  if (thread_.joinable()) { thread_.join(); }
}

// ---------------------------------------------------------------------------
// Internal thread
// ---------------------------------------------------------------------------

void Heartbeat::Run(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    Beep();

    // std::this_thread::sleep_until lets the OS wake us precisely;
    // we check the stop token after each sleep so Stop() is responsive.
    std::this_thread::sleep_for(interval_);
  }
}

// ---------------------------------------------------------------------------
// Beep
// ---------------------------------------------------------------------------

void Heartbeat::Beep() {
  try {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    callback_(nlohmann::json{
        {"status", "online"},
        {"timestamp", now_ms},
    });
  } catch (const std::exception& e) { log_.error("Beep failed: {}", e.what()); }
}

}  // namespace aember::device_health
