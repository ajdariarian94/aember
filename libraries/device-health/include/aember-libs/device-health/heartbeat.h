/**
 * @file heartbeat.h
 * @author Arian Ajdari
 * @brief Heartbeat service for device health monitoring.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <stop_token>
#include <thread>

namespace aember::device_health {

/**
 * Sends periodic heartbeat signals with a JSON payload for monitoring.
 *
 * Uses std::jthread + std::stop_token (C++20) so there is no manual
 * running_ flag, no condition variable, and no explicit join — the thread
 * is stopped and joined automatically when the Heartbeat is destroyed or
 * Stop() is called.
 *
 * The callback is move_only_function: it is captured once at construction
 * and never copied.
 */
class Heartbeat {
 public:
  using Logger = aember::utils::logging::Logger;
  using Callback = std::move_only_function<void(const nlohmann::json&)>;

  /**
   * @param callback Invoked with a JSON payload on every heartbeat tick.
   * @param interval Time between ticks (default 1000 ms).
   */
  explicit Heartbeat(Callback callback, std::chrono::milliseconds interval =
                                            std::chrono::milliseconds{1000});

  /// Stops and joins the background thread.
  ~Heartbeat();

  Heartbeat(const Heartbeat&) = delete;
  Heartbeat& operator=(const Heartbeat&) = delete;

  /// Starts the background thread. No-op if already running.
  void Start();

  /// Requests the background thread to stop and blocks until it exits.
  void Stop();

  /// Fire one heartbeat immediately (callable from any thread).
  void Beep();

 private:
  /// Thread body — receives the stop_token from jthread automatically.
  void Run(std::stop_token stop_token);

  std::chrono::milliseconds interval_;
  Callback callback_;
  std::jthread thread_;
  Logger log_{"heartbeat"};
};

}  // namespace aember::device_health
