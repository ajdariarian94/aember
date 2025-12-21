#include <aember-libs/service-manager/service-manager.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <thread>

namespace aember::service_manager {

ServiceManager::ServiceManager(
    aember::child_supervisor::ChildSupervisor& supervisor)
    : child_supervisor_(supervisor), log_("service-manager") {
  log_.info("ServiceManager initialized");
}

ServiceManager::~ServiceManager() {
  StopAll();
}

bool ServiceManager::AddService(const ServiceConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty()) {
    log_.error("Cannot add service with empty name");
    return false;
  }

  if (config.command.empty()) {
    log_.error("Cannot add service '{}' with empty command", config.name);
    return false;
  }

  if (services_.find(config.name) != services_.end()) {
    log_.warn("Service '{}' already exists", config.name);
    return false;
  }

  auto service = std::make_shared<Service>(config);
  services_[config.name] = service;

  log_.info("Added service '{}'", config.name);
  return true;
}

bool ServiceManager::RemoveService(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = services_.find(name);
  if (it == services_.end()) {
    log_.warn("Service '{}' not found", name);
    return false;
  }

  auto service = it->second;
  if (service->GetState() == ServiceState::RUNNING ||
      service->GetState() == ServiceState::STARTING) {
    log_.error("Cannot remove running service '{}'", name);
    return false;
  }

  services_.erase(it);
  log_.info("Removed service '{}'", name);
  return true;
}

bool ServiceManager::StartService(const std::string& name) {
  return StartServiceInternal(name, false);
}

bool ServiceManager::StartServiceInternal(const std::string& name,
                                          bool is_restart) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto it = services_.find(name);
  if (it == services_.end()) {
    log_.error("Service '{}' not found", name);
    return false;
  }

  auto service = it->second;
  ServiceState current_state = service->GetState();

  if (current_state == ServiceState::RUNNING) {
    log_.info("Service '{}' is already running", name);
    return true;
  }

  if (current_state == ServiceState::STARTING) {
    log_.info("Service '{}' is already starting", name);
    return true;
  }

  // Check dependencies
  if (!CheckDependencies(name)) {
    log_.error("Cannot start service '{}': dependencies not met", name);
    return false;
  }

  // Start dependencies first
  lock.unlock();
  if (!StartServiceDependencies(name)) {
    log_.error("Failed to start dependencies for service '{}'", name);
    return false;
  }
  lock.lock();

  ChangeServiceState(name, ServiceState::STARTING);

  const auto& config = service->GetConfig();

  log_.info("Starting service '{}'{}", name, is_restart ? " (restart)" : "");

  // Spawn the process
  lock.unlock();
  pid_t pid = SpawnProcess(config);
  lock.lock();

  if (pid < 0) {
    log_.error("Failed to spawn process for service '{}'", name);
    ChangeServiceState(name, ServiceState::FAILED);
    return false;
  }

  service->SetPid(pid);
  service->SetStartTime();
  if (is_restart) { service->SetLastRestartTime(); }

  // Register with child supervisor
  lock.unlock();
  child_supervisor_.AddChild(pid, name);
  lock.lock();

  pid_to_service_[pid] = name;

  ChangeServiceState(name, ServiceState::RUNNING);

  log_.info("Service '{}' started with pid {}", name, pid);
  return true;
}

bool ServiceManager::StopService(const std::string& name) {
  return StopServiceInternal(name);
}

bool ServiceManager::StopServiceInternal(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = services_.find(name);
  if (it == services_.end()) {
    log_.error("Service '{}' not found", name);
    return false;
  }

  auto service = it->second;
  ServiceState current_state = service->GetState();

  if (current_state == ServiceState::STOPPED) {
    log_.info("Service '{}' is already stopped", name);
    return true;
  }

  if (current_state == ServiceState::STOPPING) {
    log_.info("Service '{}' is already stopping", name);
    return true;
  }

  // If the service is in FAILED state or process is already dead, just mark as
  // stopped
  if (current_state == ServiceState::FAILED) {
    ChangeServiceState(name, ServiceState::STOPPED);
    service->SetPid(-1);
    return true;
  }

  ChangeServiceState(name, ServiceState::STOPPING);

  pid_t pid = service->GetPid();
  if (pid > 0) {
    log_.info("Stopping service '{}' (pid {})", name, pid);

    // Check if process is still alive first
    if (kill(pid, 0) != 0) {
      // Process is already dead
      log_.debug("Service '{}' process already exited", name);
      ChangeServiceState(name, ServiceState::STOPPED);
      service->SetPid(-1);

      // Remove from pid tracking
      auto pid_it = pid_to_service_.find(pid);
      if (pid_it != pid_to_service_.end()) { pid_to_service_.erase(pid_it); }
      return true;
    }

    // Send SIGTERM
    if (kill(pid, SIGTERM) != 0) {
      log_.warn(
          "Failed to send SIGTERM to service '{}': {}", name, strerror(errno));
      // Process might already be dead, mark as stopped
      ChangeServiceState(name, ServiceState::STOPPED);
      service->SetPid(-1);
      return true;
    }

    // Note: State will be changed to STOPPED when HandleServiceExit is called
  } else {
    ChangeServiceState(name, ServiceState::STOPPED);
  }

  return true;
}

bool ServiceManager::RestartService(const std::string& name) {
  log_.info("Restarting service '{}'", name);

  if (!StopServiceInternal(name)) { return false; }

  // Wait a bit for the service to stop
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  return StartServiceInternal(name, true);
}

void ServiceManager::StartAll() {
  log_.info("Starting all services");

  std::vector<std::string> service_names = GetServiceNames();

  for (const auto& name : service_names) { StartService(name); }
}

void ServiceManager::StopAll() {
  std::vector<std::string> service_names = GetServiceNames();

  if (service_names.empty()) {
    log_.debug("No services to stop");
    return;
  }

  log_.info("Stopping {} services", service_names.size());

  // Stop in reverse order
  for (auto it = service_names.rbegin(); it != service_names.rend(); ++it) {
    StopService(*it);
  }

  // Wait for all services to stop (with shorter timeout)
  auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool all_stopped = false;

  while (std::chrono::steady_clock::now() < timeout) {
    all_stopped = true;

    for (const auto& name : service_names) {
      auto state = GetServiceState(name);
      if (state != ServiceState::STOPPED && state != ServiceState::FAILED) {
        all_stopped = false;
        break;
      }
    }

    if (all_stopped) {
      log_.info("All services stopped successfully");
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (!all_stopped) {
    log_.warn("Some services did not stop within timeout, force stopping");

    // Force stop any remaining services
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& name : service_names) {
      auto it = services_.find(name);
      if (it != services_.end()) {
        auto service = it->second;
        if (service->GetState() != ServiceState::STOPPED &&
            service->GetState() != ServiceState::FAILED) {
          pid_t pid = service->GetPid();
          if (pid > 0) {
            log_.warn("Force killing service '{}' (pid {})", name, pid);
            kill(pid, SIGKILL);
          }

          service->SetState(ServiceState::STOPPED);
          service->SetPid(-1);
        }
      }
    }
  }
}

bool ServiceManager::HasService(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return services_.find(name) != services_.end();
}

ServiceState ServiceManager::GetServiceState(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = services_.find(name);
  if (it == services_.end()) { return ServiceState::STOPPED; }

  return it->second->GetState();
}

std::vector<std::string> ServiceManager::GetServiceNames() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> names;
  for (const auto& [name, _] : services_) { names.push_back(name); }

  return names;
}

std::shared_ptr<Service> ServiceManager::GetService(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = services_.find(name);
  if (it == services_.end()) { return nullptr; }

  return it->second;
}

void ServiceManager::HandleServiceExit(pid_t pid, int exit_code) {
  std::unique_lock<std::mutex> lock(mutex_);
  
  auto it = pid_to_service_.find(pid);
  if (it == pid_to_service_.end()) {
    log_.debug("Process {} exited but not tracked by service manager", pid);
    return;
  }
  
  std::string service_name = it->second;
  pid_to_service_.erase(it);
  
  auto service_it = services_.find(service_name);
  if (service_it == services_.end()) {
    log_.warn("Service '{}' not found for pid {}", service_name, pid);
    return;
  }
  
  auto service = service_it->second;
  service->SetExitCode(exit_code);
  service->SetPid(-1);
  
  const auto& config = service->GetConfig();
  
  // Check if service was being stopped (STOPPING state)
  bool was_stopping = (service->GetState() == ServiceState::STOPPING);
  
  // Exit codes 128-165 are typically signals (128 + signal number)
  // 143 = SIGTERM (128 + 15), 137 = SIGKILL (128 + 9)
  bool killed_by_signal = (exit_code >= 128 && exit_code <= 165);
  
  if (exit_code == 0 || (was_stopping && killed_by_signal)) {
    // Clean exit, or was intentionally stopped
    log_.info("Service '{}' exited{}", service_name, 
              killed_by_signal ? " (stopped by signal)" : " cleanly");
    ChangeServiceState(service_name, ServiceState::STOPPED);
  } else {
    log_.warn("Service '{}' exited with code {}", service_name, exit_code);
    ChangeServiceState(service_name, ServiceState::FAILED);
  }
  
  // Check if we should restart (only if not intentionally stopped)
  if (!was_stopping) {
    bool should_restart = false;
    
    if (config.restart_policy == RestartPolicy::ALWAYS) {
      should_restart = true;
    } else if (config.restart_policy == RestartPolicy::ON_FAILURE && exit_code != 0 && !killed_by_signal) {
      should_restart = true;
    }
    
    if (should_restart && service->GetRestartCount() < config.max_restart_attempts) {
      service->IncrementRestartCount();
      log_.info("Scheduling restart for service '{}' (attempt {}/{})",
                service_name, service->GetRestartCount(), config.max_restart_attempts);
      
      lock.unlock();
      ScheduleRestart(service_name);
    } else if (should_restart) {
      log_.error("Service '{}' exceeded max restart attempts ({})",
                 service_name, config.max_restart_attempts);
    }
  }
}

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_change_callback_ = callback;
}

bool ServiceManager::StartServiceDependencies(const std::string& name) {
  auto service = GetService(name);
  if (!service) return false;

  const auto& deps = service->GetConfig().dependencies;

  for (const auto& dep : deps) {
    if (!HasService(dep)) {
      log_.error("Dependency '{}' for service '{}' not found", dep, name);
      return false;
    }

    auto dep_state = GetServiceState(dep);
    if (dep_state != ServiceState::RUNNING) {
      log_.info("Starting dependency '{}' for service '{}'", dep, name);
      if (!StartService(dep)) {
        log_.error(
            "Failed to start dependency '{}' for service '{}'", dep, name);
        return false;
      }
    }
  }

  return true;
}

bool ServiceManager::CheckDependencies(const std::string& name) const {
  auto it = services_.find(name);
  if (it == services_.end()) return false;

  const auto& deps = it->second->GetConfig().dependencies;

  for (const auto& dep : deps) {
    if (!HasService(dep)) { return false; }
  }

  return true;
}

void ServiceManager::ScheduleRestart(const std::string& name) {
  auto service = GetService(name);
  if (!service) return;

  auto delay = service->GetConfig().restart_delay;

  log_.info("Restarting service '{}' in {} seconds", name, delay.count());

  // Simple delay before restart
  std::this_thread::sleep_for(delay);

  StartServiceInternal(name, true);
}

pid_t ServiceManager::SpawnProcess(const ServiceConfig& config) {
  pid_t pid = fork();

  if (pid < 0) {
    log_.error(
        "Failed to fork for service '{}': {}", config.name, strerror(errno));
    return -1;
  }

  if (pid == 0) {
    // Child process

    // Change working directory
    if (!config.working_directory.empty()) {
      if (chdir(config.working_directory.c_str()) != 0) {
        std::cerr << "Failed to change directory: " << strerror(errno)
                  << std::endl;
        _exit(1);
      }
    }

    // Set environment variables
    for (const auto& [key, value] : config.environment) {
      setenv(key.c_str(), value.c_str(), 1);
    }

    // Prepare arguments
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(config.command.c_str()));
    for (const auto& arg : config.args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Execute
    execvp(config.command.c_str(), argv.data());

    // If we get here, exec failed
    std::cerr << "Failed to exec " << config.command << ": " << strerror(errno)
              << std::endl;
    _exit(1);
  }

  // Parent process
  return pid;
}

void ServiceManager::ChangeServiceState(const std::string& name,
                                        ServiceState new_state) {
  auto it = services_.find(name);
  if (it == services_.end()) return;

  auto service = it->second;
  ServiceState old_state = service->GetState();

  if (old_state == new_state) return;

  service->SetState(new_state);

  log_.info("Service '{}' state changed: {} -> {}",
            name,
            ServiceStateToString(old_state),
            ServiceStateToString(new_state));

  if (state_change_callback_) {
    state_change_callback_(name, old_state, new_state);
  }
}

}  // namespace aember::service_manager
