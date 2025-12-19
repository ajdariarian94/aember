/**
 * @file aember-init.h
 * @author Arian Ajdari
 * @brief Library definition for Aember Init
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/child-supervisor/child-supervisor.h>
#include <aember-libs/device-health/heartbeat.h>
#include <aember-libs/mount-manager/mount-manager.h>
#include <aember-libs/utils/logging/logging.h>
#include <aember-libs/utils/signal/signal.h>

#include <nlohmann/json.hpp>  // for nlohmann::json

#include <atomic>              // for std::atomic_bool
#include <chrono>              // for std::chrono::milliseconds
#include <condition_variable>  // for std::condition_variable
#include <functional>          // for std::function
#include <mutex>               // for std::mutex
#include <thread>              // for std::thread

namespace aember::aember_init {

class AemberInit {
 public:
  /**
   * @brief Constructs a Heartbeat object.
   *
   * @param interval Time interval between heartbeats.
   * @param callback Function to be called with heartbeat data.
   */
  explicit AemberInit();

  /**
   * @brief Destructor for Heartbeat.
   */
  ~AemberInit();

  /**
   * @brief Starts AemberInit.
   */
  void Start();

  /**
   * @brief Stops AemberInit.
   */
  void Stop();

 private:
  /**
   * @brief Runs the Beep periodically.
   */
  void RunLoop();

  std::atomic_bool running_{false};
  std::thread worker_thread_;

  std::mutex mtx_;
  std::condition_variable cv_;

  std::optional<aember::device_health::Heartbeat> heartbeat_;
  void HeartbeatCallback(const nlohmann::json& heartbeat_payload);

  std::optional<aember::mount_manager::MountManager> mount_manager_;

  aember::utils::SignalHandler signal_handler_;

  aember::child_supervisor::ChildSupervisor child_supervisor_;

  aember::utils::Logger log_;
};

}  // namespace aember::aember_init
