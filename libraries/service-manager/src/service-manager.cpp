/**
 * @file service-manager.cpp
 * @author Arian Ajdari
 * @brief ServiceManager implementation — pure coordinator.
 * @version 0.4
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/service-manager.h>

#include <thread>

namespace aember::service_manager {

using ProcessState = aember::process_manager::ProcessManager::ProcessState;

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ServiceManager::ServiceManager(
    aember::process_manager::ProcessManager& process_manager,
    aember::container_manager::ContainerManager& container_manager,
    DependencyResolver& dependency_resolver,
    aember::child_supervisor::ChildSupervisor& supervisor)
    : process_manager_(process_manager),
      container_manager_(container_manager),
      dependency_resolver_(dependency_resolver),
      child_supervisor_(supervisor) {
  // Mirror ProcessManager state changes upward.
  process_manager_.SetExitCallback(
      [this](const std::string& name, pid_t /*pid*/, int /*exit_code*/) {
        std::lock_guard<std::mutex> lock(mutex_);
        MirrorState(name, process_manager_.GetState(name));
      });

  // Mirror ContainerManager state changes upward.
  container_manager_.SetStateCallback(
      [this](const std::string& name,
             aember::utils::container::ContainerState /*old*/,
             aember::utils::container::ContainerState new_state) {
        using CS = aember::utils::container::ContainerState;
        ProcessState ps = ProcessState::Stopped;
        switch (new_state) {
          case CS::kStarting:
            ps = ProcessState::Starting;
            break;
          case CS::kRunning:
            ps = ProcessState::Running;
            break;
          case CS::kStopping:
            ps = ProcessState::Stopping;
            break;
          case CS::kStopped:
            ps = ProcessState::Stopped;
            break;
          case CS::kFailed:
            ps = ProcessState::Failed;
            break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        MirrorState(name, ps);
      });

  log_.info("ServiceManager initialized");
}

ServiceManager::~ServiceManager() {
  StopAll();
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

bool ServiceManager::AddProcess(const std::string& name) {
  if (!process_manager_.HasProcess(name)) {
    log_.error("AddProcess: '{}' not registered in ProcessManager", name);
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  states_[name] = ProcessState::Stopped;

  log_.info("Registered process '{}'", name);
  return true;
}

bool ServiceManager::AddContainer(const std::string& name) {
  if (!container_manager_.HasContainer(name)) {
    log_.error("AddContainer: '{}' not registered in ContainerManager", name);
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  states_[name] = ProcessState::Stopped;
  containers_.insert(name);

  log_.info("Registered container '{}'", name);
  return true;
}

bool ServiceManager::Remove(const std::string& name) {
  StopInternal(name);

  std::lock_guard<std::mutex> lock(mutex_);

  if (!states_.count(name)) {
    log_.warn("Remove: '{}' not found", name);
    return false;
  }

  if (containers_.count(name)) {
    containers_.erase(name);
  } else {
    process_manager_.RemoveProcess(name);
  }

  states_.erase(name);
  log_.info("Removed '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ServiceManager::Start(const std::string& name) {
  return StartInternal(name, false);
}

bool ServiceManager::StartInternal(const std::string& name, bool is_restart) {
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!states_.count(name)) {
      log_.error("Start: '{}' not registered", name);
      return false;
    }

    const auto state = states_.at(name);
    if (state == ProcessState::Running || state == ProcessState::Starting) {
      return true;
    }

    // Build a name → dependencies graph from ProcessManager for the resolver.
    const auto all_configs = process_manager_.GetConfigs();
    DependencyResolver::DependencyGraph graph;
    for (const auto& cfg : all_configs) { graph[cfg.name] = cfg.dependencies; }

    if (!containers_.count(name) &&
        !dependency_resolver_.CheckDependencies(name, graph)) {
      log_.error("Dependencies not met for '{}'", name);
      return false;
    }

    MirrorState(name, ProcessState::Starting);
  }

  log_.info("Starting '{}'{}", name, is_restart ? " (restart)" : "");

  bool success = false;

  if (IsContainer(name)) {
    success = container_manager_.StartContainer(name);
  } else {
    auto pid = process_manager_.Start(name);
    success = pid.has_value();
    if (success) { child_supervisor_.AddChild(*pid, name); }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    MirrorState(name, success ? ProcessState::Running : ProcessState::Failed);
  }

  return success;
}

bool ServiceManager::Stop(const std::string& name) {
  return StopInternal(name);
}

bool ServiceManager::StopInternal(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!states_.count(name)) {
    log_.error("Stop: '{}' not registered", name);
    return false;
  }

  const auto state = states_.at(name);
  if (state == ProcessState::Stopped || state == ProcessState::Stopping) {
    return true;
  }

  MirrorState(name, ProcessState::Stopping);

  if (containers_.count(name)) {
    container_manager_.StopContainer(name);
  } else {
    process_manager_.Stop(name);
  }

  return true;
}

bool ServiceManager::Restart(const std::string& name) {
  if (!StopInternal(name)) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return StartInternal(name, true);
}

// ---------------------------------------------------------------------------
// Bulk operations
// ---------------------------------------------------------------------------

void ServiceManager::StartAll() {
  const auto all_configs = process_manager_.GetConfigs();
  DependencyResolver::DependencyGraph graph;
  for (const auto& cfg : all_configs) { graph[cfg.name] = cfg.dependencies; }
  const auto ordered = dependency_resolver_.ResolveStartOrder(graph);

  for (const auto& name : ordered) { Start(name); }

  // Start containers — they have no dependency ordering for now.
  std::vector<std::string> container_names;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    container_names.assign(containers_.begin(), containers_.end());
  }
  for (const auto& name : container_names) { Start(name); }
}

void ServiceManager::StopAll() {
  const auto names = GetNames();
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    StopInternal(*it);
  }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ServiceManager::Has(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return states_.count(name) > 0;
}

bool ServiceManager::IsContainer(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return containers_.count(name) > 0;
}

ServiceManager::ProcessState ServiceManager::GetState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = states_.find(name);
  return it != states_.end() ? it->second : ProcessState::Failed;
}

std::vector<std::string> ServiceManager::GetNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(states_.size());
  for (const auto& [name, _] : states_) { names.push_back(name); }
  return names;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

void ServiceManager::HandleExit(pid_t pid, int exit_code) {
  // Dispatch to whichever manager owns this PID.
  // State updates flow back via the callbacks wired in the constructor.
  if (!process_manager_.HandleExit(pid, exit_code)) {
    container_manager_.HandleExit(pid, exit_code);
  }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_change_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void ServiceManager::MirrorState(const std::string& name,
                                 ProcessState new_state) {
  // Caller must hold mutex_.
  auto it = states_.find(name);
  if (it == states_.end()) return;

  const auto old = it->second;
  if (old == new_state) return;

  it->second = new_state;

  log_.info("'{}': {} -> {}", name, ToString(old), ToString(new_state));

  if (state_change_callback_) { state_change_callback_(name, old, new_state); }
}

}  // namespace aember::service_manager
