/**
 * @file service-manager.cpp
 * @author Arian Ajdari
 * @brief ServiceManager implementation (processes only).
 * @version 0.6
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/service-manager/service-manager.h>

#include <algorithm>
#include <ranges>
#include <thread>

namespace aember::service_manager {

using ProcessState = aember::process_manager::ProcessManager::ProcessState;

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ServiceManager::ServiceManager(
    aember::process_manager::ProcessManager& process_manager,
    DependencyResolver& dependency_resolver,
    aember::child_supervisor::ChildSupervisor& supervisor)
    : process_manager_(process_manager),
      dependency_resolver_(dependency_resolver),
      child_supervisor_(supervisor) {
  process_manager_.SetExitCallback([this](const std::string& name, pid_t, int) {
    std::lock_guard lock{mutex_};
    ready_deadline_.erase(name);
    SetState(name, process_manager_.GetState(name));
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

bool ServiceManager::Remove(std::string_view name) {
  StopInternal(name);

  std::lock_guard lock{mutex_};
  const std::string key{name};

  if (!states_.contains(key)) {
    log_.warn("Remove: '{}' not found", name);
    return false;
  }

  process_manager_.RemoveProcess(name);
  states_.erase(key);
  waiting_.erase(key);
  ready_deadline_.erase(key);
  log_.info("Removed '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ServiceManager::Start(std::string_view name) {
  return StartInternal(name, false);
}

DependencyResolver::DependencyGraph ServiceManager::Graph() const {
  DependencyResolver::DependencyGraph graph;
  for (const auto& cfg : process_manager_.GetConfigs()) {
    graph[cfg.name] = cfg.dependencies;
  }
  return graph;
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
  }

  // Without the lock: the resolver asks whether each dependency is running,
  // and that query (GetState) takes mutex_ (std::mutex, not recursive).
  if (!dependency_resolver_.CheckDependencies(key, Graph())) {
    // Remembered: it starts as soon as its dependencies run (also for
    // restarts, which happen while a dependency may itself be restarting).
    log_.info("'{}' waits for its dependencies", name);
    std::lock_guard lock{mutex_};
    if (states_.contains(key)) { waiting_.insert(key); }
    return false;
  }

  const auto config = process_manager_.GetConfig(key);
  {
    std::lock_guard lock{mutex_};
    // Another thread may have started (or removed) it meanwhile.
    const auto it = states_.find(key);
    if (it == states_.end()) { return false; }
    if (it->second == ProcessState::Running ||
        it->second == ProcessState::Starting) {
      return true;
    }
    waiting_.erase(key);
    SetState(key, ProcessState::Starting);
  }

  log_.info("Starting '{}'{}", name, is_restart ? " (restart)" : "");

  const auto pid = process_manager_.Start(name);
  const bool success = pid.has_value();
  if (success) { child_supervisor_.AddChild(*pid, key); }

  const bool needs_ready = success && config && config->ipc;
  {
    std::lock_guard lock{mutex_};
    if (needs_ready) {
      ready_deadline_[key] =
          std::chrono::steady_clock::now() + config->ready_timeout;
    } else {
      SetState(key, success ? ProcessState::Running : ProcessState::Failed);
    }
  }

  if (success && !needs_ready) { StartWaiting(); }
  return success;
}

void ServiceManager::MarkReady(std::string_view name) {
  {
    std::lock_guard lock{mutex_};
    const std::string key{name};
    if (ready_deadline_.erase(key) == 0) {
      log_.debug("'{}' ready, but no start was waiting for it", name);
      return;
    }
    log_.info("'{}' is ready", name);
    SetState(key, ProcessState::Running);
  }
  StartWaiting();
}

bool ServiceManager::AwaitingReady() const {
  std::lock_guard lock{mutex_};
  return !ready_deadline_.empty();
}

void ServiceManager::CheckReadyTimeouts() {
  bool changed = false;
  {
    std::lock_guard lock{mutex_};
    const auto now = std::chrono::steady_clock::now();
    for (auto it = ready_deadline_.begin(); it != ready_deadline_.end();) {
      if (now < it->second) {
        ++it;
        continue;
      }
      log_.warn("'{}' didn't report ready in time; treating it as up",
                it->first);
      SetState(it->first, ProcessState::Running);
      it = ready_deadline_.erase(it);
      changed = true;
    }
  }
  if (changed) { StartWaiting(); }
}

void ServiceManager::StartWaiting() {
  std::vector<std::string> candidates;
  {
    std::lock_guard lock{mutex_};
    candidates.assign(waiting_.begin(), waiting_.end());
  }
  for (const auto& name : candidates) { StartInternal(name, false); }
}

bool ServiceManager::Stop(std::string_view name) {
  return StopInternal(name);
}

bool ServiceManager::StopInternal(std::string_view name) {
  const std::string key{name};
  {
    std::lock_guard lock{mutex_};

    if (!states_.contains(key)) {
      log_.error("Stop: '{}' not registered", name);
      return false;
    }

    waiting_.erase(key);
    ready_deadline_.erase(key);
    const auto state = states_.at(key);
    if (state == ProcessState::Stopped || state == ProcessState::Stopping) {
      return true;
    }

    SetState(key, ProcessState::Stopping);
  }

  // Without the lock: ProcessManager reports the exit through a callback
  // that takes mutex_ (std::mutex, not recursive).
  process_manager_.Stop(name);

  // Nothing was running (failed, or a restart pending): no exit will be
  // reported, so it is stopped now.
  if (!process_manager_.GetPid(name)) {
    std::lock_guard lock{mutex_};
    const auto it = states_.find(key);
    if (it != states_.end() && it->second == ProcessState::Stopping) {
      SetState(key, ProcessState::Stopped);
    }
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
  process_manager_.EnableRestarts();
  const auto order = dependency_resolver_.ResolveStartOrder(Graph());
  {
    std::lock_guard lock{mutex_};
    for (const auto& name : order) {
      if (states_.contains(name)) { waiting_.insert(name); }
    }
  }
  for (const auto& name : order) {
    if (Has(name)) { StartInternal(name, false); }
  }
}

void ServiceManager::StopAll() {
  // First: a restart pending now would otherwise fire during (or after) the
  // shutdown and start its service again, or call into a destroyed
  // ServiceManager (spec/tla/StartGraph.tla, NoStartAfterShutdown).
  process_manager_.CancelRestarts();
  {
    std::lock_guard lock{mutex_};
    waiting_.clear();
  }
  // Dependents first: the reverse of the start order, then anything the
  // resolver left out (cycles, unknown dependencies).
  auto order = dependency_resolver_.ResolveStartOrder(Graph());
  std::ranges::reverse(order);
  for (const auto& name : GetNames()) {
    if (std::ranges::find(order, name) == order.end()) {
      order.push_back(name);
    }
  }
  for (const auto& name : order) {
    if (Has(name)) { StopInternal(name); }
  }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ServiceManager::Has(std::string_view name) const {
  std::lock_guard lock{mutex_};
  return states_.contains(std::string{name});
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
  process_manager_.HandleExit(pid, exit_code);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void ServiceManager::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard lock{mutex_};
  state_change_callback_ = std::move(callback);
}

void ServiceManager::SetState(const std::string& name, ProcessState new_state) {
  auto it = states_.find(name);
  if (it == states_.end()) { return; }

  const auto old = it->second;
  if (old == new_state) { return; }

  it->second = new_state;

  log_.info("'{}': {} -> {}", name, ToString(old), ToString(new_state));

  if (state_change_callback_) { state_change_callback_(name, old, new_state); }
}

}  // namespace aember::service_manager
