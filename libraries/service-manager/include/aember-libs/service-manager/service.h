#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aember::service_manager {

enum class ServiceState { STOPPED, STARTING, RUNNING, STOPPING, FAILED };

enum class RestartPolicy { NEVER, ON_FAILURE, ALWAYS };

struct ServiceConfig {
  std::string name;
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> environment;
  std::string working_directory;
  RestartPolicy restart_policy = RestartPolicy::NEVER;
  std::vector<std::string> dependencies;
  int max_restart_attempts = 5;
  std::chrono::seconds restart_delay{5};

  ServiceConfig() = default;
  ServiceConfig(const std::string& n, const std::string& cmd)
      : name(n), command(cmd) {}
};

class Service {
 public:
  Service(const ServiceConfig& config);

  const std::string& GetName() const { return config_.name; }
  const ServiceConfig& GetConfig() const { return config_; }
  ServiceState GetState() const;
  pid_t GetPid() const;
  int GetExitCode() const { return exit_code_; }
  int GetRestartCount() const { return restart_count_; }
  std::chrono::system_clock::time_point GetStartTime() const {
    return start_time_;
  }
  std::chrono::system_clock::time_point GetLastRestartTime() const {
    return last_restart_time_;
  }

  void SetState(ServiceState state);
  void SetPid(pid_t pid);
  void SetExitCode(int code) { exit_code_ = code; }
  void IncrementRestartCount() { restart_count_++; }
  void ResetRestartCount() { restart_count_ = 0; }
  void SetStartTime() { start_time_ = std::chrono::system_clock::now(); }
  void SetLastRestartTime() {
    last_restart_time_ = std::chrono::system_clock::now();
  }

 private:
  ServiceConfig config_;
  ServiceState state_ = ServiceState::STOPPED;
  pid_t pid_ = -1;
  int exit_code_ = 0;
  int restart_count_ = 0;
  std::chrono::system_clock::time_point start_time_;
  std::chrono::system_clock::time_point last_restart_time_;
  mutable std::mutex mutex_;
};

// Helper functions
std::string ServiceStateToString(ServiceState state);
std::string RestartPolicyToString(RestartPolicy policy);

}  // namespace aember::service_manager
