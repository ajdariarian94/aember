/**
 * @file service.h
 * @author Arian Ajdari
 * @brief Service and ServiceConfig definitions for Aember Service Manager
 * @version 0.2
 * @date 2026-03-24
 *
 * @copyright Copyright (c) 2025-2026, Aember, All rights reserved.
 */
#pragma once

#include <aember-libs/utils/service/restart-policy.h>
#include <aember-libs/utils/service/service-config.h>
#include <aember-libs/utils/service/service-state.h>
#include <aember-libs/utils/service/service-type.h>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>  // for pid_t

namespace aember::service_manager {

/**
 * @brief Represents a running service and tracks its state.
 *
 * Provides getters and setters for managing service state, PID, exit code, and
 * restart history.
 */
class Service {
 public:
 
 using ServiceConfig = aember::utils::service::ServiceConfig;
 using ServiceState = aember::utils::service::ServiceState;

  /**
   * @brief Constructs a Service with the given configuration.
   */
  Service(const ServiceConfig& config);

  /** @brief Returns the service name. */
  const std::string& GetName() const { return config_.name; }

  /** @brief Returns the service configuration. */
  const ServiceConfig& GetConfig() const {
    return config_;
  }

  /** @brief Returns the current service state. */
  ServiceState GetState() const;

  /** @brief Returns the PID of the running process, or -1 if not running. */
  pid_t GetPid() const;

  /** @brief Returns the last exit code of the service. */
  int GetExitCode() const;

  /** @brief Returns how many times the service has been restarted. */
  int GetRestartCount() const;

  /** @brief Returns the timestamp when the service was last started. */
  std::chrono::system_clock::time_point GetStartTime() const;

  /** @brief Returns the timestamp of the last automatic restart. */
  std::chrono::system_clock::time_point GetLastRestartTime() const;

  /** @brief Set the service state. */
  void SetState(ServiceState state);

  /** @brief Set the PID of the service process. */
  void SetPid(pid_t pid);

  /** @brief Set the exit code of the service process. */
  void SetExitCode(int code);

  /** @brief Increment the restart counter. */
  void IncrementRestartCount();

  /** @brief Reset the restart counter to zero. */
  void ResetRestartCount();

  /** @brief Update the start timestamp to now. */
  void SetStartTime();

  /** @brief Update the last restart timestamp to now. */
  void SetLastRestartTime();

 private:
 ServiceConfig config_;  ///< Service configuration
 ServiceState state_ = ServiceState::STOPPED;  ///< Current state
  pid_t pid_ = -1;                                    ///< Process ID
  int exit_code_ = 0;                                 ///< Last exit code
  int restart_count_ = 0;                             ///< Restart counter
  std::chrono::system_clock::time_point start_time_;  ///< Start time
  std::chrono::system_clock::time_point
      last_restart_time_;     ///< Last restart time
  mutable std::mutex mutex_;  ///< Protects access to state and counters
};

}  // namespace aember::service_manager
