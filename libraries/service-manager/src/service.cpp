/**
 * @file service.cpp
 * @author Arian Ajdari
 * @brief Implementation of Service class and helper functions for Aember
 * Service Manager
 * @version 0.2
 * @date 2026-03-24
 *
 * @copyright Copyright (c) 2025-2026, Aember, All rights reserved.
 */
#include <aember-libs/service-manager/service.h>
#include <aember-libs/utils/logging/logging.h>

namespace aember::service_manager {

/**
 * @brief Constructs a Service object with the given configuration.
 *
 * @param config Service configuration structure
 */
Service::Service(const ServiceConfig& config) : config_(config) {
  aember::utils::logging::Logger log("service");
  log.info("Service '{}' created (type: {})",
           config_.name,
           config_.type == ServiceType::PROCESS ? "PROCESS" : "CONTAINER");
}

/**
 * @brief Get the current state of the service.
 *
 * Thread-safe.
 *
 * @return ServiceState Current state
 */
aember::utils::service::ServiceState Service::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

/**
 * @brief Get the PID of the service process.
 *
 * Thread-safe.
 *
 * @return pid_t Process ID, or -1 if not running
 */
pid_t Service::GetPid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pid_;
}

/**
 * @brief Get the last exit code of the service.
 *
 * Thread-safe.
 *
 * @return int Exit code
 */
int Service::GetExitCode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return exit_code_;
}

/**
 * @brief Get how many times the service has been restarted.
 *
 * Thread-safe.
 *
 * @return int Restart count
 */
int Service::GetRestartCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return restart_count_;
}

/**
 * @brief Get the service start timestamp.
 *
 * Thread-safe.
 *
 * @return std::chrono::system_clock::time_point Start time
 */
std::chrono::system_clock::time_point Service::GetStartTime() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return start_time_;
}

/**
 * @brief Get the last automatic restart timestamp.
 *
 * Thread-safe.
 *
 * @return std::chrono::system_clock::time_point Last restart time
 */
std::chrono::system_clock::time_point Service::GetLastRestartTime() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_restart_time_;
}

/**
 * @brief Set the state of the service.
 *
 * Thread-safe.
 *
 * @param state New service state
 */
void Service::SetState(aember::utils::service::ServiceState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

/**
 * @brief Set the PID of the service process.
 *
 * Thread-safe.
 *
 * @param pid Process ID
 */
void Service::SetPid(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  pid_ = pid;
}

/**
 * @brief Set the exit code of the service process.
 *
 * Thread-safe.
 *
 * @param code Exit code
 */
void Service::SetExitCode(int code) {
  std::lock_guard<std::mutex> lock(mutex_);
  exit_code_ = code;
}

/**
 * @brief Increment the restart counter.
 *
 * Thread-safe.
 */
void Service::IncrementRestartCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  restart_count_++;
}

/**
 * @brief Reset the restart counter to zero.
 *
 * Thread-safe.
 */
void Service::ResetRestartCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  restart_count_ = 0;
}

/**
 * @brief Update the start timestamp to now.
 *
 * Thread-safe.
 */
void Service::SetStartTime() {
  std::lock_guard<std::mutex> lock(mutex_);
  start_time_ = std::chrono::system_clock::now();
}

/**
 * @brief Update the last restart timestamp to now.
 *
 * Thread-safe.
 */
void Service::SetLastRestartTime() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_restart_time_ = std::chrono::system_clock::now();
}

}  // namespace aember::service_manager
