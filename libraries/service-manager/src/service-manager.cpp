/**
 * @file service-manager.cpp
 * @author Arian Ajdari
 * @brief ServiceManager implementation — pure coordinator.
 * @version 0.5
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/service-manager.h>

#include <ranges>
#include <thread>

namespace aember::service_manager {

using ProcessState = aember::process_manager::ProcessManager::ProcessState;

// Lookup table: ContainerState → ProcessState (avoids switch in constructor).
static constexpr auto kContainerToProcess = [] {
  using CS = aember::utils::container::ContainerState;
  return std::array{
      std::pair{CS::kStarting, ProcessState::Starting},
      std::pair{CS::kRunning, ProcessState::Running},
      std::pair{CS::kStopping, ProcessState::Stopping},
      std::pair{CS::kStopped, ProcessState::Stopped},
      std::pair{CS::kFailed, ProcessState::Failed},
  };
}();

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
  process_manager_.SetExitCallback([this](const std::string& name, pid_t, int) {
    std::lock_guard lock{mutex_};
    MirrorState(name, process_manager_.GetState(name));
  });

  container_manager_.SetStateCallback(
      [this](const std::string& name,
             aember::utils::container::ContainerState,
             aember::utils::container::ContainerState new_state) {
        ProcessState ps = ProcessState::Stopped;
        for (const auto& [cs, mapped] : kContainerToProcess) {
          if (cs == new_state) {
            ps = mapped;
            break;
          }
        }
        std::lock_guard lock{mutex_};
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

bool ServiceManager::AddProcess(std::string_view name) {
  if (!process_manager_.HasProcess(name)) {
    log_.error("AddProcess: '{}' not registered in ProcessManager", name);
    return false;
  }

  std::lock_guard lock{mutex_};
  states_[std::string{name}] = ProcessState::Stopped;
  log_.info("Registered process '{}'", name);
  return true;
}

bool ServiceManager::AddContainer(std::string_view name) {
  if (!container_manager_.HasContainer(name)) {
    log_.error("AddContainer: '{}' not registered in ContainerManager", name);
    return false;
  }

  std::lock_guard lock{mutex_};
  const std::string key{name};

  // Guard against duplicate addition inside ServiceManager
  if (containers_.contains(key)) {
    log_.warn(
        "AddContainer: Container '{}' already registered in ServiceManager",
        name);
    return false;
  }

  states_[key] = ProcessState::Stopped;
  containers_.insert(key);
  log_.info("Registered container '{}'", name);
  return true;
}

bool ServiceManager::Remove(std::string_view name) {
  StopInternal(name);

  std::lock_guard lock{mutex_};
  const std::string key{name};

  if (!states_.contains(key)) {
    log_.warn("Remove: '{}' not found", name);
    return false;
  }

  if (containers_.contains(key)) {
    containers_.erase(key);
  } else {
    process_manager_.RemoveProcess(name);
  }

  states_.erase(key);
  log_.info("Removed '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ServiceManager::Start(std::string_view name) {
  return StartInternal(name, false);
}

bool ServiceManager::StartInternal(std::string_view name, bool is_restart) {
  const std::string key{name};

  {
    std::lock_guard lock{mutex_};

    if (!states_.contains(key)) {
      log_.error("Start: '{}' not registered", name);
      return false;
    }

    const auto state = states_.at(key);
    if (state == ProcessState::Running || state == ProcessState::Starting) {
      return true;
    }

    if (!containers_.contains(key)) {
      const auto all_configs = process_manager_.GetConfigs();
      DependencyResolver::DependencyGraph graph;
      for (const auto& cfg : all_configs) {
        graph[cfg.name] = cfg.dependencies;
      }

      if (!dependency_resolver_.CheckDependencies(key, graph)) {
        log_.error("Dependencies not met for '{}'", name);
        return false;
      }
    }

    MirrorState(key, ProcessState::Starting);
  }

  log_.info("Starting '{}'{}", name, is_restart ? " (restart)" : "");

  bool success = false;

  if (IsContainer(name)) {
    success = container_manager_.StartContainer(name);
  } else {
    auto pid = process_manager_.Start(name);
    success = pid.has_value();
    if (success) { child_supervisor_.AddChild(*pid, key); }
  }

  {
    std::lock_guard lock{mutex_};
    MirrorState(key, success ? ProcessState::Running : ProcessState::Failed);
  }

  return success;
}

bool ServiceManager::Stop(std::string_view name) {
  return StopInternal(name);
}

bool ServiceManager::StopInternal(std::string_view name) {
  std::lock_guard lock{mutex_};
  const std::string key{name};

  if (!states_.contains(key)) {
    log_.error("Stop: '{}' not registered", name);
    return false;
  }

  const auto state = states_.at(key);
  if (state == ProcessState::Stopped || state == ProcessState::Stopping) {
    return true;
  }

  MirrorState(key, ProcessState::Stopping);

  if (containers_.contains(key)) {
    container_manager_.StopContainer(name);
  } else {
    process_manager_.Stop(name);
  }

  return true;
}

bool ServiceManager::Restart(std::string_view name) {
  if (!StopInternal(name)) { return false; }
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  return StartInternal(name, true);
}

// ---------------------------------------------------------------------------
// Bulk operations
// ---------------------------------------------------------------------------

void ServiceManager::StartAll() {
  const auto all_configs = process_manager_.GetConfigs();
  DependencyResolver::DependencyGraph graph;
  for (const auto& cfg : all_configs) { graph[cfg.name] = cfg.dependencies; }

  for (const auto& name : dependency_resolver_.ResolveStartOrder(graph)) {
    Start(name);
  }

  std::vector<std::string> container_names;
  {
    std::lock_guard lock{mutex_};
    container_names.assign(containers_.begin(), containers_.end());
  }
  for (const auto& name : container_names) { Start(name); }
}

void ServiceManager::StopAll() {
  const auto names = GetNames();
  for (const auto& name : names | std::views::reverse) { StopInternal(name); }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ServiceManager::Has(std::string_view name) const {
  std::lock_guard lock{mutex_};
  return states_.contains(std::string{name});
}

bool ServiceManager::IsContainer(std::string_view name) const {
  std::lock_guard lock{mutex_};
  return containers_.contains(std::string{name});
}

ServiceManager::ProcessState ServiceManager::GetState(
    std::string_view name) const {
  std::lock_guard lock{mutex_};
  auto it = states_.find(std::string{name});
  return it != states_.end() ? it->second : ProcessState::Failed;
}

std::vector<std::string> ServiceManager::GetNames() const {
  std::lock_guard lock{mutex_};
  std::vector<std::string> names;
  names.reserve(states_.size());
  std::ranges::transform(states_,
                         std::back_inserter(names),
                         [](const auto& kv) { return kv.first; });
  return names;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

void ServiceManager::HandleExit(pid_t pid, int exit_code) {
  if (!process_manager_.HandleExit(pid, exit_code)) {
    container_manager_.HandleExit(pid, exit_code);
  }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard lock{mutex_};
  state_change_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void ServiceManager::MirrorState(const std::string& name,
                                 ProcessState new_state) {
  auto it = states_.find(name);
  if (it == states_.end()) { return; }

  const auto old = it->second;
  if (old == new_state) { return; }

  it->second = new_state;

  log_.info("'{}': {} -> {}", name, ToString(old), ToString(new_state));

  if (state_change_callback_) { state_change_callback_(name, old, new_state); }
}

}  // namespace aember::service_manager
