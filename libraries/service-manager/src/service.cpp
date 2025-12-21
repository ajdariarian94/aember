#include <aember-libs/service-manager/service.h>

namespace aember::service_manager {

Service::Service(const ServiceConfig& config) : config_(config) {}

ServiceState Service::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

pid_t Service::GetPid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pid_;
}

void Service::SetState(ServiceState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

void Service::SetPid(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  pid_ = pid;
}

// Helper functions
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
