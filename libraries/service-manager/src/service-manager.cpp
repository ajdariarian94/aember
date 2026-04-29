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
    aember::child_supervisor::ChildSupervisor& supervisor)
    : child_supervisor_(supervisor), log_("service-manager") {
  log_.info("ServiceManager initialized");
}

ServiceManager::~ServiceManager() {
  StopAll();
}

// --------------------------
// Add / Remove Services
// --------------------------

bool ServiceManager::AddService(
    const aember::utils::service::ServiceConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty()) {
    log_.error("Cannot add service with empty name");
    return false;
  }

  if (config.type == aember::utils::service::ServiceType::PROCESS &&
      config.command.empty()) {
    log_.error("Cannot add process service '{}' with empty command",
               config.name);
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

// --------------------------
// Container Registration
// --------------------------

bool ServiceManager::AddContainer(
    const aember::utils::container::ContainerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty()) {
    log_.error("Cannot add container with empty name");
    return false;
  }

  if (containers_.find(config.name) != containers_.end()) {
    log_.warn("Container '{}' already exists", config.name);
    return false;
  }

  containers_[config.name] = config;

  log_.info("Registered container '{}'", config.name);
  return true;
}

std::vector<aember::utils::container::ContainerConfig>
ServiceManager::GetContainers() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<aember::utils::container::ContainerConfig> result;
  result.reserve(containers_.size());

  for (const auto& [name, config] : containers_) { result.push_back(config); }

  return result;
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
// Start / Stop / Restart
// --------------------------

bool ServiceManager::StartService(const std::string& name) {
  return StartServiceInternal(name, false);
}

bool ServiceManager::StartServiceInternal(const std::string& name,
                                          bool is_restart) {
  std::vector<std::string> dependencies;
  aember::utils::service::ServiceConfig config_copy;
  aember::utils::service::ServiceType type;

  {
    std::unique_lock<std::mutex> lock(mutex_);

    auto it = services_.find(name);
    if (it == services_.end()) {
      log_.error("Service '{}' not found", name);
      return false;
    }

    auto service = it->second;

    if (service->GetState() == aember::utils::service::ServiceState::RUNNING ||
        service->GetState() == aember::utils::service::ServiceState::STARTING) {
      return true;
    }

    if (!CheckDependencies(name)) {
      log_.error("Dependencies not met for '{}'", name);
      return false;
    }

    dependencies = service->GetConfig().dependencies;
    config_copy = service->GetConfig();
    type = config_copy.type;

    ChangeServiceState(name, aember::utils::service::ServiceState::STARTING);
  }

  for (const auto& dep : dependencies) { StartService(dep); }

  log_.info("Starting service '{}'{}", name, is_restart ? " (restart)" : "");

  pid_t pid = -1;
  bool success = false;

  if (type == aember::utils::service::ServiceType::PROCESS) {
    pid = SpawnProcess(config_copy);
    success = (pid >= 0);
  } else if (type == aember::utils::service::ServiceType::CONTAINER) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (containers_.find(name) == containers_.end()) {
        log_.error("Container '{}' not registered", name);
        success = false;
      }
    }

    if (!container_state_callback_) {
      log_.error("No container callback set");
      success = false;
    } else {
      success = container_state_callback_(
          name, aember::utils::container::ContainerState::kRunning);
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

    if (type == aember::utils::service::ServiceType::PROCESS) {
      service->SetPid(pid);
      pid_to_service_[pid] = name;
    } else {
      service->SetPid(-1);
    }

    service->SetStartTime();

    if (is_restart) {
      service->IncrementRestartCount();
      service->SetLastRestartTime();
    }

    ChangeServiceState(name, aember::utils::service::ServiceState::RUNNING);
  }

  if (type == aember::utils::service::ServiceType::PROCESS) {
    child_supervisor_.AddChild(pid, name);
  }

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

  if (service->GetState() == aember::utils::service::ServiceState::STOPPED ||
      service->GetState() == aember::utils::service::ServiceState::STOPPING) {
    return true;
  }

  ChangeServiceState(name, aember::utils::service::ServiceState::STOPPING);

  if (service->GetConfig().type ==
      aember::utils::service::ServiceType::PROCESS) {
    pid_t pid = service->GetPid();
    if (pid > 0) { kill(pid, SIGTERM); }
    service->SetPid(-1);
  } else {
    if (container_state_callback_) {
      container_state_callback_(
          name, aember::utils::container::ContainerState::kStopped);
    }
  }

  ChangeServiceState(name, aember::utils::service::ServiceState::STOPPED);

  return true;
}

bool ServiceManager::RestartService(const std::string& name) {
  if (!StopServiceInternal(name)) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return StartServiceInternal(name, true);
}

// --------------------------
// Bulk
// --------------------------

void ServiceManager::StartAll() {
  // 1. start services
  for (const auto& name : GetServiceNames()) { StartService(name); }

  // 2. explicitly trigger container lifecycle
  for (const auto& [name, config] : containers_) {
    log_.info("Triggering container start: {}", name);

    if (container_state_callback_) {
      container_state_callback_(
          name, aember::utils::container::ContainerState::kStarting);
    }
  }
}

std::vector<std::string> ServiceManager::GetServiceNames() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> names;
  names.reserve(services_.size());

  for (const auto& [name, service] : services_) { names.push_back(name); }

  return names;
}

bool ServiceManager::CheckDependencies(const std::string& name) const {
  auto it = services_.find(name);
  if (it == services_.end()) { return false; }

  const auto& deps = it->second->GetConfig().dependencies;

  for (const auto& dep : deps) {
    if (services_.find(dep) == services_.end()) {
      log_.error("Missing dependency '{}' for service '{}'", dep, name);
      return false;
    }
  }

  return true;
}

void ServiceManager::StopAll() {
  auto names = GetServiceNames();
  for (auto it = names.rbegin(); it != names.rend(); ++it) { StopService(*it); }
}

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_change_callback_ = std::move(callback);
}

void ServiceManager::HandleServiceExit(pid_t pid, int exit_code) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto it = pid_to_service_.find(pid);
  if (it == pid_to_service_.end()) { return; }

  const std::string service_name = it->second;
  pid_to_service_.erase(it);

  auto service_it = services_.find(service_name);
  if (service_it == services_.end()) { return; }

  auto service = service_it->second;
  service->SetPid(-1);
  service->SetExitCode(exit_code);

  const auto state = service->GetState();
  const auto was_stopping =
      (state == aember::utils::service::ServiceState::STOPPING);

  if (exit_code == 0 || was_stopping) {
    ChangeServiceState(service_name,
                       aember::utils::service::ServiceState::STOPPED);
  } else {
    ChangeServiceState(service_name,
                       aember::utils::service::ServiceState::FAILED);
  }
}

// --------------------------
// Queries
// --------------------------

bool ServiceManager::HasService(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return services_.find(name) != services_.end();
  ;
}

// --------------------------
// Callbacks
// --------------------------

void ServiceManager::SetContainerStateCallback(
    ContainerStateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  container_state_callback_ = callback;
}

// --------------------------

std::vector<ServiceManager::ServiceConfig> ServiceManager::LoadServices(
    const std::string& name) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(name, error)) {
    log_.error("Failed to load services: {}", error.message);
    return {};
  }

  return parser_.GetServices();
}

// --------------------------

pid_t ServiceManager::SpawnProcess(
    const aember::utils::service::ServiceConfig& config) {
  pid_t pid = fork();

  if (pid == 0) {
    if (!config.working_directory.empty()) {
      chdir(config.working_directory.c_str());
    }

    for (const auto& [k, v] : config.environment) {
      setenv(k.c_str(), v.c_str(), 1);
    }

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(config.command.c_str()));

    for (const auto& arg : config.args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }

    argv.push_back(nullptr);

    execvp(config.command.c_str(), argv.data());
    _exit(1);
  }

  return pid;
}

// --------------------------

void ServiceManager::ChangeServiceState(
    const std::string& name, aember::utils::service::ServiceState new_state) {
  auto it = services_.find(name);
  if (it == services_.end()) return;

  auto service = it->second;
  auto old = service->GetState();

  if (old == new_state) return;

  service->SetState(new_state);

  if (state_change_callback_) { state_change_callback_(name, old, new_state); }
}

}  // namespace aember::service_manager
