/**
 * @file service-manager.cpp
 * @author Arian Ajdari
 * @brief Implementation of ServiceManager
 * @version 0.2
 * @date 2026-03-24
 *
 * @copyright Copyright (c) 2025-2026, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/service-manager.h>
#include <aember-libs/utils/logging/logging.h>

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

// --------------------------
// Constructor / Destructor
// --------------------------

ServiceManager::ServiceManager(
    aember::child_supervisor::ChildSupervisor& supervisor,
    std::shared_ptr<aember::container_manager::ContainerManager>
        container_manager)
    : child_supervisor_(supervisor),
      container_manager_(std::move(container_manager)),
      log_("service-manager") {
  log_.info("ServiceManager initialized");
}

ServiceManager::~ServiceManager() {
  StopAll();  // Ensure all services are stopped on destruction
}

// --------------------------
// Add / Remove Services
// --------------------------

bool ServiceManager::AddService(const ServiceConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty()) {
    log_.error("Cannot add service with empty name");
    return false;
  }

  if (config.type == ServiceType::PROCESS && config.command.empty()) {
    log_.error("Cannot add process service '{}' with empty command",
               config.name);
    return false;
  }

  if (services_.find(config.name) != services_.end()) {
    log_.warn("Service '{}' already exists", config.name);
    return false;
  }

  // Register container with ContainerManager if type is CONTAINER
  if (config.type == ServiceType::CONTAINER) {
    if (!container_manager_) {
      log_.error(
          "Cannot add container service '{}': ContainerManager not initialized",
          config.name);
      return false;
    }

    if (!config.container.has_value()) {
      log_.error("Container service '{}' has no container spec", config.name);
      return false;
    }

    aember::container_manager::ContainerConfig cc;
    cc.name = config.name;
    cc.rootfs = config.container->rootfs;
    cc.args = config.container->args;
    cc.squashfs = config.container->squashfs;

    if (!container_manager_->AddContainer(cc)) {
      log_.error("Failed to register container '{}' with ContainerManager",
                 config.name);
      return false;
    }

    log_.info("Registered container '{}' with ContainerManager", config.name);
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
  if (service->GetState() == aember::utils::service::ServiceState::RUNNING ||
      service->GetState() == aember::utils::service::ServiceState::STARTING) {
    log_.error("Cannot remove running service '{}'", name);
    return false;
  }

  services_.erase(it);
  log_.info("Removed service '{}'", name);
  return true;
}

// --------------------------
// Start / Stop / Restart Services
// --------------------------

bool ServiceManager::StartService(const std::string& name) {
  return StartServiceInternal(name, false);
}

bool ServiceManager::StartServiceInternal(const std::string& name,
                                          bool is_restart) {
  std::vector<std::string> dependencies;
  ServiceConfig config_copy;
  ServiceType type;

  {
    std::unique_lock<std::mutex> lock(mutex_);

    auto it = services_.find(name);
    if (it == services_.end()) {
      log_.error("Service '{}' not found", name);
      return false;
    }

    auto service = it->second;
    auto state = service->GetState();

    if (state == aember::utils::service::ServiceState::RUNNING ||
        state == aember::utils::service::ServiceState::STARTING) {
      return true;
    }

    if (!CheckDependencies(name)) {
      log_.error("Cannot start service '{}': dependencies not met", name);
      return false;
    }

    dependencies = service->GetConfig().dependencies;
    config_copy = service->GetConfig();
    type = config_copy.type;

    ChangeServiceState(name, aember::utils::service::ServiceState::STARTING);
  }

  // Start dependencies outside mutex
  for (const auto& dep : dependencies) { StartService(dep); }

  log_.info("Starting service '{}'{}", name, is_restart ? " (restart)" : "");

  pid_t pid = -1;
  bool success = false;

  if (type == ServiceType::PROCESS) {
    pid = SpawnProcess(config_copy);
    success = (pid >= 0);
  } else if (type == ServiceType::CONTAINER) {
    if (!container_manager_) {
      log_.error("ContainerManager not initialized");
      success = false;
    } else {
      success = container_manager_->StartContainer(name);
    }
  }

  if (!success) {
    std::lock_guard<std::mutex> lock(mutex_);
    ChangeServiceState(name, aember::utils::service::ServiceState::FAILED);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto service = services_[name];

    // Only processes have real PIDs
    if (type == ServiceType::PROCESS) {
      service->SetPid(pid);
      pid_to_service_[pid] = name;
    } else {
      // Containers do not use PID tracking here
      service->SetPid(-1);
    }

    service->SetStartTime();

    if (is_restart) {
      service->SetLastRestartTime();
      service->IncrementRestartCount();
    }

    ChangeServiceState(name, aember::utils::service::ServiceState::RUNNING);
  }

  if (type == ServiceType::PROCESS) { child_supervisor_.AddChild(pid, name); }

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
  aember::utils::service::ServiceState current_state = service->GetState();

  if (current_state == aember::utils::service::ServiceState::STOPPED ||
      current_state == aember::utils::service::ServiceState::STOPPING) {
    log_.info("Service '{}' already stopped or stopping", name);
    return true;
  }

  ChangeServiceState(name, aember::utils::service::ServiceState::STOPPING);

  if (service->GetConfig().type == ServiceType::PROCESS) {
    pid_t pid = service->GetPid();
    if (pid > 0) {
      log_.info("Stopping process service '{}' (pid {})", name, pid);
      if (kill(pid, 0) != 0 || kill(pid, SIGTERM) != 0) {
        log_.warn("Failed to send SIGTERM to service '{}', considering stopped",
                  name);
      }
    }
    service->SetPid(-1);

  } else if (service->GetConfig().type == ServiceType::CONTAINER) {
    log_.info("Stopping container service '{}'", name);
    if (container_manager_) {
      if (!container_manager_->StopContainer(name)) {
        log_.warn("ContainerManager failed to stop container '{}'", name);
      }
    } else {
      log_.warn("ContainerManager not available, cannot stop container '{}'",
                name);
    }
    service->SetPid(-1);
  }

  ChangeServiceState(name, aember::utils::service::ServiceState::STOPPED);
  return true;
}

bool ServiceManager::RestartService(const std::string& name) {
  log_.info("Restarting service '{}'", name);

  if (!StopServiceInternal(name)) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return StartServiceInternal(name, true);
}

// --------------------------
// Bulk Operations
// --------------------------

void ServiceManager::StartAll() {
  log_.info("Starting all services");
  for (const auto& name : GetServiceNames()) { StartService(name); }
}

void ServiceManager::StopAll() {
  auto service_names = GetServiceNames();
  if (service_names.empty()) return;

  log_.info("Stopping {} services", service_names.size());
  for (auto it = service_names.rbegin(); it != service_names.rend(); ++it) {
    StopService(*it);
  }
}

// --------------------------
// Queries
// --------------------------

bool ServiceManager::HasService(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return services_.find(name) != services_.end();
}

aember::utils::service::ServiceState ServiceManager::GetServiceState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = services_.find(name);
  return (it != services_.end())
             ? it->second->GetState()
             : aember::utils::service::ServiceState::STOPPED;
}

std::vector<std::string> ServiceManager::GetServiceNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  for (const auto& [name, _] : services_) names.push_back(name);
  return names;
}

std::shared_ptr<Service> ServiceManager::GetService(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = services_.find(name);
  return (it != services_.end()) ? it->second : nullptr;
}

// --------------------------
// Process Exit Handling
// --------------------------

void ServiceManager::HandleServiceExit(pid_t pid, int exit_code) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = pid_to_service_.find(pid);
  if (it == pid_to_service_.end()) return;

  std::string service_name = it->second;
  pid_to_service_.erase(it);

  auto service_it = services_.find(service_name);
  if (service_it == services_.end()) return;

  auto service = service_it->second;
  service->SetExitCode(exit_code);
  service->SetPid(-1);

  const auto& config = service->GetConfig();
  bool was_stopping =
      (service->GetState() == aember::utils::service::ServiceState::STOPPING);
  bool killed_by_signal = (exit_code >= 128 && exit_code <= 165);

  if (exit_code == 0 || (was_stopping && killed_by_signal)) {
    ChangeServiceState(service_name,
                       aember::utils::service::ServiceState::STOPPED);
  } else {
    ChangeServiceState(service_name,
                       aember::utils::service::ServiceState::FAILED);
  }

  if (!was_stopping) {
    bool should_restart = false;
    if (config.restart_policy == aember::utils::service::RestartPolicy::ALWAYS)
      should_restart = true;
    if (config.restart_policy ==
            aember::utils::service::RestartPolicy::ON_FAILURE &&
        exit_code != 0 && !killed_by_signal)
      should_restart = true;

    if (should_restart &&
        service->GetRestartCount() < config.max_restart_attempts) {
      service->IncrementRestartCount();
      lock.unlock();
      ScheduleRestart(service_name);
    }
  }
}

// --------------------------
// State Change
// --------------------------

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_change_callback_ = callback;
}

// --------------------------
// Dependency Management
// --------------------------

bool ServiceManager::StartServiceDependencies(const std::string& name) {
  auto service = GetService(name);
  if (!service) return false;

  for (const auto& dep : service->GetConfig().dependencies) {
    if (!HasService(dep)) return false;
    if (GetServiceState(dep) != aember::utils::service::ServiceState::RUNNING) {
      if (!StartService(dep)) return false;
    }
  }
  return true;
}

bool ServiceManager::CheckDependencies(const std::string& name) const {
  auto it = services_.find(name);
  if (it == services_.end()) return false;

  for (const auto& dep : it->second->GetConfig().dependencies)
    if (services_.find(dep) == services_.end()) return false;

  return true;
}

// --------------------------
// Restart Scheduling
// --------------------------

void ServiceManager::ScheduleRestart(const std::string& name) {
  auto service = GetService(name);
  if (!service) return;

  auto delay = service->GetConfig().restart_delay;
  std::thread([this, name, delay]() {
    std::this_thread::sleep_for(delay);
    StartServiceInternal(name, true);
  }).detach();
}

// --------------------------
// Process Spawning
// --------------------------

pid_t ServiceManager::SpawnProcess(const ServiceConfig& config) {
  pid_t pid = fork();

  if (pid < 0) {
    log_.error("Failed to fork for service '{}'", config.name);
    return -1;
  }

  if (pid == 0) {
    // Child process
    if (!config.working_directory.empty())
      chdir(config.working_directory.c_str());
    for (const auto& [key, value] : config.environment)
      setenv(key.c_str(), value.c_str(), 1);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(config.command.c_str()));
    for (const auto& arg : config.args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    execvp(config.command.c_str(), argv.data());
    _exit(1);  // exec failed
  }

  return pid;
}

// --------------------------
// Internal State Change
// --------------------------

void ServiceManager::ChangeServiceState(
    const std::string& name, aember::utils::service::ServiceState new_state) {
  auto it = services_.find(name);
  if (it == services_.end()) return;

  auto service = it->second;
  aember::utils::service::ServiceState old_state = service->GetState();
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
