/**
 * @file service.cpp
 * @author Arian Ajdari
 * @brief Implementation of Service class and helper functions for Aember
 * Service Manager
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */
#include <aember-libs/service-manager/service.h>

namespace aember::service_manager {

/**
 * @brief Constructs a Service object with the given configuration.
 *
 * @param config Service configuration structure
 */
Service::Service(const ServiceConfig& config) : config_(config) {}

/**
 * @brief Get the current state of the service.
 *
 * Thread-safe.
 *
 * @return ServiceState Current state
 */
ServiceState Service::GetState() const {
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
 * @brief Set the state of the service.
 *
 * Thread-safe.
 *
 * @param state New service state
 */
void Service::SetState(ServiceState state) {
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
 * @brief Convert a ServiceState enum to a human-readable string.
 *
 * @param state ServiceState value
 * @return std::string Human-readable string
 */
std::string ServiceStateToString(ServiceState state) {
  switch (state) {
    case ServiceState::STOPPED:
      return "STOPPED";
    case ServiceState::STARTING:
      return "STARTING";
    case ServiceState::RUNNING:
      return "RUNNING";
    case ServiceState::STOPPING:
      return "STOPPING";
    case ServiceState::FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief Convert a RestartPolicy enum to a human-readable string.
 *
 * @param policy RestartPolicy value
 * @return std::string Human-readable string
 */
std::string RestartPolicyToString(RestartPolicy policy) {
  switch (policy) {
    case RestartPolicy::NEVER:
      return "NEVER";
    case RestartPolicy::ON_FAILURE:
      return "ON_FAILURE";
    case RestartPolicy::ALWAYS:
      return "ALWAYS";
    default:
      return "UNKNOWN";
  }
}

}  // namespace aember::service_manager
