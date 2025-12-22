/**
 * @file heartbeat.h
 * @author Arian Ajdari
 * @brief Library definition for Heartbeat service used for device health
 * monitoring.
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace aember::device_health {

/**
 * @class Heartbeat
 * @brief Sends periodic heartbeat signals with JSON payload for monitoring
 * purposes.
 */
class Heartbeat {
 public:
  /**
   * @brief Constructs a Heartbeat object.
   * @param callback Function to be called with heartbeat JSON payload.
   * @param interval Time interval between heartbeats (default 1000ms).
   */
  explicit Heartbeat(const std::function<void(const nlohmann::json&)>& callback,
                     const std::chrono::milliseconds& interval =
                         std::chrono::milliseconds(1000));

  /**
   * @brief Destructor. Stops the heartbeat thread if running.
   */
  ~Heartbeat();

  /**
   * @brief Starts the heartbeat background thread.
   * If already running, this does nothing.
   */
  void Start();

  /**
   * @brief Stops the heartbeat background thread.
   * Blocks until the thread exits.
   */
  void Stop();

  /**
   * @brief Sends a heartbeat immediately by calling the callback.
   */
  void Beep();

 private:
  /**
   * @brief Internal thread function that periodically sends heartbeats.
   */
  void Run();

  std::atomic_bool running_{false};     ///< True if heartbeat thread is running
  std::chrono::milliseconds interval_;  ///< Time interval between heartbeats
  std::function<void(const nlohmann::json&)>
      callback_;              ///< Callback function for heartbeats
  std::thread check_thread_;  ///< Background thread for heartbeats
  std::condition_variable
      cv_;            ///< Condition variable for thread synchronization
  std::mutex mutex_;  ///< Mutex for thread synchronization
  aember::utils::Logger log_;  ///< Logger instance
};

}  // namespace aember::device_health
