/**
 * @file process-manager.cpp
 * @author Arian Ajdari
 * @brief ProcessManager implementation — native processes only.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/process-manager/process-manager.h>

#include <ranges>
#include <thread>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aember::process_manager {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ProcessManager::ProcessManager(ExitCallback on_exit)
    : on_exit_(std::move(on_exit)) {
  log_.info("ProcessManager initialized");
}

ProcessManager::~ProcessManager() {
  std::lock_guard lock{mutex_};
  for (const auto& [name, pid] : name_to_pid_) {
    log_.info("Killing '{}' (pid {})", name, pid);
    kill(pid, SIGKILL);
  }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

bool ProcessManager::AddProcess(const ProcessConfig& config) {
  std::lock_guard lock{mutex_};

  if (config.name.empty()) {
    log_.error("Cannot add process with empty name");
    return false;
  }

  if (config.executable.empty()) {
    log_.error("Cannot add process '{}' with empty executable", config.name);
    return false;
  }

  if (configs_.contains(config.name)) {
    log_.warn("Process '{}' already registered", config.name);
    return false;
  }

  configs_[config.name] = config;
  states_[config.name] = ProcessState::Stopped;
  restart_counts_[config.name] = 0;

  log_.info("Registered process '{}' ({})", config.name, config.executable);
  return true;
}

bool ProcessManager::RemoveProcess(std::string_view name) {
  Stop(name);  // acquires its own lock

  std::lock_guard lock{mutex_};
  const std::string key{name};

  if (!configs_.contains(key)) {
    log_.warn("RemoveProcess: '{}' not found", name);
    return false;
  }

  configs_.erase(key);
  states_.erase(key);
  restart_counts_.erase(key);

  log_.info("Removed process '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::optional<pid_t> ProcessManager::Start(std::string_view name) {
  std::lock_guard lock{mutex_};
  const std::string key{name};

  auto cfg_it = configs_.find(key);
  if (cfg_it == configs_.end()) {
    log_.error("Start: '{}' not registered", name);
    return std::nullopt;
  }

  const auto state = states_.at(key);
  if (state == ProcessState::Running || state == ProcessState::Starting) {
    log_.warn("Process '{}' already running", name);
    return name_to_pid_.contains(key)
               ? std::optional<pid_t>{name_to_pid_.at(key)}
               : std::nullopt;
  }

  states_[key] = ProcessState::Starting;

  const pid_t pid = SpawnProcess(cfg_it->second);
  if (pid < 0) {
    log_.error("Failed to spawn '{}'", name);
    states_[key] = ProcessState::Failed;
    return std::nullopt;
  }

  pid_to_name_[pid] = key;
  name_to_pid_[key] = pid;
  states_[key] = ProcessState::Running;

  log_.info("Started '{}' (pid {})", name, pid);
  return pid;
}

bool ProcessManager::Stop(std::string_view name) {
  std::unique_lock lock{mutex_};
  const std::string key{name};

  auto it = name_to_pid_.find(key);
  if (it == name_to_pid_.end()) { return true; }

  const pid_t pid = it->second;
  const auto stop_timeout = configs_.contains(key)
                                ? configs_.at(key).stop_timeout
                                : std::chrono::milliseconds{5000};

  states_[key] = ProcessState::Stopping;
  log_.info("Stopping '{}' (pid {}) — SIGTERM", name, pid);
  kill(pid, SIGTERM);

  // Release lock while polling so HandleExit can acquire it.
  lock.unlock();

  const auto deadline = std::chrono::steady_clock::now() + stop_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(pid, nullptr, WNOHANG) == pid) {
      log_.info("'{}' exited after SIGTERM", name);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  log_.warn("'{}' did not exit after SIGTERM — SIGKILL", name);
  kill(pid, SIGKILL);
  return true;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

bool ProcessManager::HandleExit(pid_t pid, int exit_code) {
  std::unique_lock lock{mutex_};

  auto it = pid_to_name_.find(pid);
  if (it == pid_to_name_.end()) { return false; }

  const std::string name = std::move(it->second);
  pid_to_name_.erase(it);
  name_to_pid_.erase(name);

  const bool was_stopping = (states_.at(name) == ProcessState::Stopping);
  const bool clean_exit = WIFEXITED(exit_code) && (WEXITSTATUS(exit_code) == 0);

  if (was_stopping || clean_exit) {
    states_[name] = ProcessState::Stopped;
    log_.info("'{}' (pid {}) stopped (exit_code={})", name, pid, exit_code);
  } else {
    states_[name] = ProcessState::Failed;
    log_.warn("'{}' (pid {}) exited unexpectedly (exit_code={})",
              name,
              pid,
              exit_code);
  }

  // Release lock before firing callback — callback may call back into us.
  lock.unlock();

  if (on_exit_) { on_exit_(name, pid, exit_code); }

  if (!was_stopping && !clean_exit) { ScheduleRestart(name); }

  return true;
}

// ---------------------------------------------------------------------------
// Restart scheduling
// ---------------------------------------------------------------------------

void ProcessManager::ScheduleRestart(std::string_view name) {
  std::unique_lock lock{mutex_};
  const std::string key{name};

  if (!configs_.contains(key)) { return; }

  const auto& config = configs_.at(key);

  if (!config.restart_on_failure) {
    log_.info("Restart disabled for '{}'", name);
    return;
  }

  auto& count = restart_counts_.at(key);

  if (config.max_restarts > 0 && count >= config.max_restarts) {
    log_.error(
        "'{}' reached max restarts ({}), giving up", name, config.max_restarts);
    states_[key] = ProcessState::Failed;
    return;
  }

  ++count;
  states_[key] = ProcessState::Restarting;

  const auto delay = config.restart_delay;
  log_.info(
      "Scheduling restart #{} for '{}' in {}ms", count, name, delay.count());

  lock.unlock();

  // jthread auto-joins on destruction — detach replaced with move-to-local.
  std::jthread{[this, key, delay](std::stop_token) {
    std::this_thread::sleep_for(delay);
    log_.info("Restarting '{}'", key);
    Start(key);
  }}.detach();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ProcessManager::HasProcess(std::string_view name) const {
  std::lock_guard lock{mutex_};
  return configs_.contains(std::string{name});
}

bool ProcessManager::IsRunning(std::string_view name) const {
  std::lock_guard lock{mutex_};
  auto it = states_.find(std::string{name});
  return it != states_.end() && it->second == ProcessState::Running;
}

ProcessManager::ProcessState ProcessManager::GetState(
    std::string_view name) const {
  std::lock_guard lock{mutex_};
  auto it = states_.find(std::string{name});
  return it != states_.end() ? it->second : ProcessState::Failed;
}

std::optional<pid_t> ProcessManager::GetPid(std::string_view name) const {
  std::lock_guard lock{mutex_};
  auto it = name_to_pid_.find(std::string{name});
  return it != name_to_pid_.end() ? std::optional<pid_t>{it->second}
                                  : std::nullopt;
}

std::vector<std::string> ProcessManager::GetProcessNames() const {
  std::lock_guard lock{mutex_};
  std::vector<std::string> names;
  names.reserve(configs_.size());
  std::ranges::transform(configs_,
                         std::back_inserter(names),
                         [](const auto& kv) { return kv.first; });
  return names;
}

std::vector<ProcessManager::ProcessConfig> ProcessManager::GetConfigs() const {
  std::lock_guard lock{mutex_};
  std::vector<ProcessConfig> result;
  result.reserve(configs_.size());
  std::ranges::transform(configs_,
                         std::back_inserter(result),
                         [](const auto& kv) { return kv.second; });
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void ProcessManager::SetExitCallback(ExitCallback callback) {
  std::lock_guard lock{mutex_};
  on_exit_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

std::vector<ProcessManager::ProcessConfig> ProcessManager::LoadProcesses(
    std::string_view source) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(std::string{source}, error)) {
    log_.error("Failed to load processes from '{}': {}", source, error.message);
    return {};
  }

  log_.info("Loaded process configs from '{}'", source);
  return parser_.GetProcesses();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

pid_t ProcessManager::SpawnProcess(const ProcessConfig& config) {
  const pid_t pid = fork();

  if (pid < 0) {
    log_.error("fork() failed for '{}': {}", config.name, strerror(errno));
    return -1;
  }

  if (pid == 0) {
    if (!config.working_directory.empty()) {
      chdir(config.working_directory.c_str());
    }

    for (const auto& [key, value] : config.environment) {
      setenv(key.c_str(), value.c_str(), 1);
    }

    std::vector<char*> argv;
    argv.reserve(config.args.size() + 2);
    argv.push_back(const_cast<char*>(config.executable.c_str()));
    std::ranges::transform(
        config.args, std::back_inserter(argv), [](const auto& a) {
          return const_cast<char*>(a.c_str());
        });
    argv.push_back(nullptr);

    execvp(config.executable.c_str(), argv.data());
    _exit(1);
  }

  return pid;
}

}  // namespace aember::process_manager
