/**
 * @file service.h
 * @author Arian Ajdari
 * @brief Service and ServiceConfig definitions for Aember Service Manager
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aember::service_manager {

/**
 * @brief Possible states a service can be in.
 */
enum class ServiceState {
  STOPPED,   ///< Service is not running
  STARTING,  ///< Service is starting up
  RUNNING,   ///< Service is currently running
  STOPPING,  ///< Service is stopping
  FAILED     ///< Service failed (crashed or exited with error)
};

/**
 * @brief Defines when a service should be restarted after it stops.
 */
enum class RestartPolicy {
  NEVER,       ///< Do not restart automatically
  ON_FAILURE,  ///< Restart only if the service failed
  ALWAYS       ///< Always restart regardless of exit code
};

/**
 * @brief Configuration data for a service.
 */
struct ServiceConfig {
  std::string name;                                ///< Service name
  std::string command;                             ///< Command to execute
  std::vector<std::string> args;                   ///< Command-line arguments
  std::map<std::string, std::string> environment;  ///< Environment variables
  std::string working_directory;  ///< Working directory for the service
  RestartPolicy restart_policy = RestartPolicy::NEVER;  ///< Restart behavior
  std::vector<std::string>
      dependencies;              ///< Services that must start before this one
  int max_restart_attempts = 5;  ///< Maximum number of automatic restarts
  std::chrono::seconds restart_delay{5};  ///< Delay between restarts

  ServiceConfig() = default;
  ServiceConfig(const std::string& n, const std::string& cmd)
      : name(n), command(cmd) {}
};

/**
 * @brief Represents a running service and tracks its state.
 *
 * Provides getters and setters for managing service state, PID, exit code, and
 * restart history.
 */
class Service {
 public:
  /**
   * @brief Constructs a Service with the given configuration.
   */
  Service(const ServiceConfig& config);

  /** @brief Returns the service name. */
  const std::string& GetName() const { return config_.name; }

  /** @brief Returns the service configuration. */
  const ServiceConfig& GetConfig() const { return config_; }

  /** @brief Returns the current service state. */
  ServiceState GetState() const;

  /** @brief Returns the PID of the running process, or -1 if not running. */
  pid_t GetPid() const;

  /** @brief Returns the last exit code of the service. */
  int GetExitCode() const { return exit_code_; }

  /** @brief Returns how many times the service has been restarted. */
  int GetRestartCount() const { return restart_count_; }

  /** @brief Returns the timestamp when the service was last started. */
  std::chrono::system_clock::time_point GetStartTime() const {
    return start_time_;
  }

  /** @brief Returns the timestamp of the last automatic restart. */
  std::chrono::system_clock::time_point GetLastRestartTime() const {
    return last_restart_time_;
  }

  /** @brief Set the service state. */
  void SetState(ServiceState state);

  /** @brief Set the PID of the service process. */
  void SetPid(pid_t pid);

  /** @brief Set the exit code of the service process. */
  void SetExitCode(int code) { exit_code_ = code; }

  /** @brief Increment the restart counter. */
  void IncrementRestartCount() { restart_count_++; }

  /** @brief Reset the restart counter to zero. */
  void ResetRestartCount() { restart_count_ = 0; }

  /** @brief Update the start timestamp to now. */
  void SetStartTime() { start_time_ = std::chrono::system_clock::now(); }

  /** @brief Update the last restart timestamp to now. */
  void SetLastRestartTime() {
    last_restart_time_ = std::chrono::system_clock::now();
  }

 private:
  ServiceConfig config_;                              ///< Service configuration
  ServiceState state_ = ServiceState::STOPPED;        ///< Current state
  pid_t pid_ = -1;                                    ///< Process ID
  int exit_code_ = 0;                                 ///< Last exit code
  int restart_count_ = 0;                             ///< Restart counter
  std::chrono::system_clock::time_point start_time_;  ///< Start time
  std::chrono::system_clock::time_point
      last_restart_time_;     ///< Last restart time
  mutable std::mutex mutex_;  ///< Protects access to state and counters
};

/**
 * @brief Convert a ServiceState enum to a human-readable string.
 */
std::string ServiceStateToString(ServiceState state);

/**
 * @brief Convert a RestartPolicy enum to a human-readable string.
 */
std::string RestartPolicyToString(RestartPolicy policy);

}  // namespace aember::service_manager
