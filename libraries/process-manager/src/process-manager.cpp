/**
 * @file process-manager.cpp
 * @author Arian Ajdari
 * @brief ProcessManager implementation — native processes only.
 * @version 0.1
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/process-manager/process-manager.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>

namespace aember::process_manager {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ProcessManager::ProcessManager(ExitCallback on_exit)
    : on_exit_(std::move(on_exit)) {
  log_.info("ProcessManager initialized");
}

ProcessManager::~ProcessManager() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto& [name, pid] : name_to_pid_) {
    log_.info("Killing process '{}' (pid {})", name, pid);
    kill(pid, SIGKILL);
  }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

bool ProcessManager::AddProcess(const ProcessConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty()) {
    log_.error("Cannot add process with empty name");
    return false;
  }

  if (config.executable.empty()) {
    log_.error("Cannot add process '{}' with empty executable", config.name);
    return false;
  }

  if (configs_.count(config.name)) {
    log_.warn("Process '{}' already registered", config.name);
    return false;
  }

  configs_[config.name] = config;
  states_[config.name] = ProcessState::Stopped;
  restart_counts_[config.name] = 0;

  log_.info("Registered process '{}' ({})", config.name, config.executable);
  return true;
}

bool ProcessManager::RemoveProcess(const std::string& name) {
  // Stop outside the lock since Stop() acquires it.
  Stop(name);

  std::lock_guard<std::mutex> lock(mutex_);

  if (!configs_.count(name)) {
    log_.warn("RemoveProcess: '{}' not found", name);
    return false;
  }

  configs_.erase(name);
  states_.erase(name);
  restart_counts_.erase(name);

  log_.info("Removed process '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::optional<pid_t> ProcessManager::Start(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto cfg_it = configs_.find(name);
  if (cfg_it == configs_.end()) {
    log_.error("Start: process '{}' not registered", name);
    return std::nullopt;
  }

  const auto state = states_.at(name);
  if (state == ProcessState::Running || state == ProcessState::Starting) {
    log_.warn("Process '{}' already running", name);
    return name_to_pid_.count(name)
               ? std::optional<pid_t>(name_to_pid_.at(name))
               : std::nullopt;
  }

  states_[name] = ProcessState::Starting;

  pid_t pid = SpawnProcess(cfg_it->second);
  if (pid < 0) {
    log_.error("Failed to spawn process '{}'", name);
    states_[name] = ProcessState::Failed;
    return std::nullopt;
  }

  pid_to_name_[pid] = name;
  name_to_pid_[name] = pid;
  states_[name] = ProcessState::Running;

  log_.info("Started process '{}' (pid {})", name, pid);
  return pid;
}

bool ProcessManager::Stop(const std::string& name) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto it = name_to_pid_.find(name);
  if (it == name_to_pid_.end()) {
    // Not running — nothing to do.
    return true;
  }

  const pid_t pid = it->second;
  const auto stop_timeout = configs_.count(name)
                                ? configs_.at(name).stop_timeout
                                : std::chrono::milliseconds(5000);

  states_[name] = ProcessState::Stopping;

  log_.info("Stopping process '{}' (pid {}) — sending SIGTERM", name, pid);
  kill(pid, SIGTERM);

  // Release the lock while we wait so HandleExit can acquire it if the
  // process dies quickly.
  lock.unlock();

  // Poll for exit up to stop_timeout, then SIGKILL.
  const auto deadline = std::chrono::steady_clock::now() + stop_timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(pid, nullptr, WNOHANG) == pid) {
      log_.info("Process '{}' exited cleanly after SIGTERM", name);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  log_.warn("Process '{}' did not exit after SIGTERM — sending SIGKILL", name);
  kill(pid, SIGKILL);

  return true;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

bool ProcessManager::HandleExit(pid_t pid, int exit_code) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto it = pid_to_name_.find(pid);
  if (it == pid_to_name_.end()) {
    // Not our PID — caller should try ContainerManager.
    return false;
  }

  const std::string name = it->second;

  pid_to_name_.erase(it);
  name_to_pid_.erase(name);

  const bool was_stopping = (states_.at(name) == ProcessState::Stopping);
  const bool clean_exit = WIFEXITED(exit_code) && (WEXITSTATUS(exit_code) == 0);

  if (was_stopping || clean_exit) {
    states_[name] = ProcessState::Stopped;
    log_.info(
        "Process '{}' (pid {}) stopped (exit_code={})", name, pid, exit_code);
  } else {
    states_[name] = ProcessState::Failed;
    log_.warn("Process '{}' (pid {}) exited unexpectedly (exit_code={})",
              name,
              pid,
              exit_code);
  }

  // Fire the exit callback before deciding to restart so ServiceManager
  // can update its own state first.
  const auto cb = on_exit_;
  lock.unlock();

  if (cb) { cb(name, pid, exit_code); }

  // Restart policy — only on unexpected exit.
  if (!was_stopping && !clean_exit) { ScheduleRestart(name); }

  return true;
}

// ---------------------------------------------------------------------------
// Restart scheduling
// ---------------------------------------------------------------------------

void ProcessManager::ScheduleRestart(const std::string& name) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!configs_.count(name)) return;

  const auto& config = configs_.at(name);

  if (!config.restart_on_failure) {
    log_.info("Restart disabled for '{}'", name);
    return;
  }

  auto& count = restart_counts_.at(name);

  if (config.max_restarts > 0 && count >= config.max_restarts) {
    log_.error("Process '{}' reached max restarts ({}), giving up",
               name,
               config.max_restarts);
    states_[name] = ProcessState::Failed;
    return;
  }

  ++count;
  states_[name] = ProcessState::Restarting;

  const auto delay = config.restart_delay;

  log_.info(
      "Scheduling restart #{} for '{}' in {}ms", count, name, delay.count());

  lock.unlock();

  // Fire the restart on a detached thread so we don't block the caller.
  std::thread([this, name, delay]() {
    std::this_thread::sleep_for(delay);
    log_.info("Restarting '{}'", name);
    Start(name);
  }).detach();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ProcessManager::HasProcess(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return configs_.count(name) > 0;
}

bool ProcessManager::IsRunning(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = states_.find(name);
  return it != states_.end() && it->second == ProcessState::Running;
}

ProcessManager::ProcessState ProcessManager::GetState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = states_.find(name);
  return it != states_.end() ? it->second : ProcessState::Failed;
}

std::optional<pid_t> ProcessManager::GetPid(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = name_to_pid_.find(name);
  return it != name_to_pid_.end() ? std::optional<pid_t>(it->second)
                                  : std::nullopt;
}

std::vector<std::string> ProcessManager::GetProcessNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(configs_.size());
  for (const auto& [name, _] : configs_) { names.push_back(name); }
  return names;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void ProcessManager::SetExitCallback(ExitCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_exit_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

std::vector<ProcessManager::ProcessConfig> ProcessManager::LoadProcesses(
    const std::string& source) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(source, error)) {
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
  pid_t pid = fork();

  if (pid < 0) {
    log_.error("fork() failed for '{}': {}", config.name, strerror(errno));
    return -1;
  }

  if (pid == 0) {
    // Child process.

    if (!config.working_directory.empty()) {
      if (chdir(config.working_directory.c_str()) != 0) {
        log_.error("chdir to '{}' failed", config.working_directory);
      }
    }

    for (const auto& [key, value] : config.environment) {
      setenv(key.c_str(), value.c_str(), 1);
    }

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(config.executable.c_str()));
    for (const auto& arg : config.args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(config.executable.c_str(), argv.data());

    // execvp only returns on failure.
    _exit(1);
  }

  // Parent — return the child PID.
  return pid;
}

std::vector<ProcessManager::ProcessConfig> ProcessManager::GetConfigs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ProcessConfig> result;
  result.reserve(configs_.size());
  for (const auto& [_, cfg] : configs_) { result.push_back(cfg); }
  return result;
}

}  // namespace aember::process_manager
