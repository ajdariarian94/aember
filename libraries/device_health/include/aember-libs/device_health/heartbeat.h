/**
 * @file heartbeat.h
 * @author Arian Ajdari
 * @brief Library definition for Heartbeat
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <nlohmann/json.hpp>  // for nlohmann::json

#include <atomic>              // for std::atomic_bool
#include <chrono>              // for std::chrono::milliseconds
#include <condition_variable>  // for std::condition_variable
#include <functional>          // for std::function
#include <mutex>               // for std::mutex
#include <thread>              // for std::thread

namespace aember::device_health {

class Heartbeat {
 public:
  /**
   * @brief Constructs a Heartbeat object.
   *
   * @param interval Time interval between heartbeats.
   * @param callback Function to be called with heartbeat data.
   */
  explicit Heartbeat(
      const std::function<void(const nlohmann::json&)>& callback,
      const std::chrono::milliseconds& interval = std::chrono::milliseconds(1000));

  /**
   * @brief Destructor for Heartbeat.
   */
  ~Heartbeat();

  /**
   * @brief Starts the Heartbeat background thread.
   */
  void Start();

  /**
   * @brief Stops the Heartbeat background thread.
   */
  void Stop();

  /**
   * @brief Sends a heartbeat.
   */
  void Beep();

 private:
  /**
   * @brief Runs the Beep periodically.
   */
  void Run();

  std::atomic_bool running_{false};
  std::chrono::milliseconds interval_;
  std::function<void(const nlohmann::json&)> callback_;
  std::thread check_thread_;
  std::condition_variable cv_;
  std::mutex mutex_;
};

}  // namespace aember::device_health
