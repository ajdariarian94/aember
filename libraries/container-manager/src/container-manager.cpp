/**
 * @file container-manager.cpp
 * @author Arian Ajdari
 * @brief ContainerManager implementation using LXC.
 * @version 0.2
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/container-manager/container-manager.h>

#include <lxc/lxccontainer.h>

#include <sys/stat.h>
#include <unistd.h>

namespace aember::container_manager {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ContainerManager::ContainerManager(
    std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
    StateCallback cb)
    : mount_manager_(std::move(mount_manager)), callback_(std::move(cb)) {
  log_.info("ContainerManager initialized");
}

ContainerManager::~ContainerManager() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto& e : containers_) {
    Stop(e);
    Release(e);
  }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

bool ContainerManager::AddContainer(const ContainerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty() || config.rootfs.empty()) {
    log_.error("Invalid container config — name and rootfs are required");
    return false;
  }

  if (Find(config.name)) {
    log_.warn("Container '{}' already registered", config.name);
    return false;
  }

  ContainerEntry e;
  e.config = config;
  containers_.push_back(std::move(e));

  log_.info(
      "Registered container '{}' (rootfs={})", config.name, config.rootfs);
  return true;
}

bool ContainerManager::RemoveContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = containers_.begin(); it != containers_.end(); ++it) {
    if (it->config.name != name) continue;

    // Clean up pid_to_name_ entry if the container had a running init PID.
    if (it->lxc && it->lxc->is_running(it->lxc)) {
      pid_t init_pid = it->lxc->init_pid(it->lxc);
      if (init_pid > 0) { pid_to_name_.erase(init_pid); }
    }

    Stop(*it);
    Release(*it);
    containers_.erase(it);

    log_.info("Removed container '{}'", name);
    return true;
  }

  log_.warn("RemoveContainer: '{}' not found", name);
  return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ContainerManager::StartContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* e = Find(name);
  if (!e) {
    log_.error("StartContainer: '{}' not found", name);
    return false;
  }

  if (e->state == ContainerState::kRunning) {
    log_.warn("Container '{}' already running", name);
    return true;
  }

  log_.info("Starting container '{}'", name);
  SetState(*e, ContainerState::kStarting);

  if (!Create(*e)) {
    SetState(*e, ContainerState::kFailed);
    return false;
  }

  if (!Start(*e)) {
    SetState(*e, ContainerState::kFailed);
    return false;
  }

  // Record the host-visible init PID so HandleExit can route SIGCHLD events.
  if (e->lxc) {
    pid_t init_pid = e->lxc->init_pid(e->lxc);
    if (init_pid > 0) {
      pid_to_name_[init_pid] = name;
      log_.debug("Container '{}' init PID: {}", name, init_pid);
    } else {
      log_.warn(
          "Container '{}' started but init_pid() returned {}", name, init_pid);
    }
  }

  SetState(*e, ContainerState::kRunning);
  return true;
}

bool ContainerManager::StopContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* e = Find(name);
  if (!e) {
    log_.error("StopContainer: '{}' not found", name);
    return false;
  }

  if (e->state == ContainerState::kStopped ||
      e->state == ContainerState::kStopping) {
    return true;
  }

  // Remove the init PID mapping before we stop.
  if (e->lxc) {
    pid_t init_pid = e->lxc->init_pid(e->lxc);
    if (init_pid > 0) { pid_to_name_.erase(init_pid); }
  }

  log_.info("Stopping container '{}'", name);
  SetState(*e, ContainerState::kStopping);

  bool ok = Stop(*e);
  Release(*e);

  SetState(*e, ok ? ContainerState::kStopped : ContainerState::kFailed);
  return ok;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

bool ContainerManager::HandleExit(pid_t pid, int exit_code) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = pid_to_name_.find(pid);
  if (it == pid_to_name_.end()) {
    // Not our PID — caller should try ProcessManager.
    return false;
  }

  const std::string name = it->second;
  pid_to_name_.erase(it);

  auto* e = Find(name);
  if (!e) {
    // Entry was already removed — nothing more to do.
    return true;
  }

  const bool was_stopping = (e->state == ContainerState::kStopping);

  Release(*e);

  if (was_stopping || exit_code == 0) {
    SetState(*e, ContainerState::kStopped);
    log_.info(
        "Container '{}' (pid {}) stopped (exit_code={})", name, pid, exit_code);
  } else {
    SetState(*e, ContainerState::kFailed);
    log_.warn("Container '{}' (pid {}) exited unexpectedly (exit_code={})",
              name,
              pid,
              exit_code);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool ContainerManager::HasContainer(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Find(name) != nullptr;
}

ContainerManager::ContainerState ContainerManager::GetContainerState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto* e = Find(name);
  return e ? e->state : ContainerState::kFailed;
}

bool ContainerManager::IsRunning(const std::string& name) const {
  return GetContainerState(name) == ContainerState::kRunning;
}

std::optional<pid_t> ContainerManager::GetInitPid(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto* e = Find(name);
  if (!e || !e->lxc) return std::nullopt;

  pid_t init_pid = e->lxc->init_pid(e->lxc);
  return init_pid > 0 ? std::optional<pid_t>(init_pid) : std::nullopt;
}

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------

void ContainerManager::SetStateCallback(StateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

std::vector<ContainerManager::ContainerConfig> ContainerManager::LoadContainers(
    const std::string& source) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(source, error)) {
    log_.error(
        "Failed to load containers from '{}': {}", source, error.message);
    return {};
  }

  log_.info("Loaded container configs from '{}'", source);
  return parser_.GetContainers();
}

// ---------------------------------------------------------------------------
// LXC lifecycle helpers
// ---------------------------------------------------------------------------

bool ContainerManager::Create(ContainerEntry& e) {
  Release(e);

  log_.debug("Creating container '{}' (squashfs='{}', rootfs='{}')",
             e.config.name,
             e.config.squashfs,
             e.config.rootfs);

  e.lxc = lxc_container_new(e.config.name.c_str(), "/tmp");
  if (!e.lxc) {
    log_.error("lxc_container_new failed for '{}'", e.config.name);
    return false;
  }

  // Mount SquashFS if specified.
  if (!e.config.squashfs.empty()) {
    if (!mount_manager_->MountSquashFS(e.config.squashfs, e.config.rootfs)) {
      log_.error("Failed to mount squashfs '{}' at '{}'",
                 e.config.squashfs,
                 e.config.rootfs);
      return false;
    }
    log_.info(
        "Mounted squashfs '{}' at '{}'", e.config.squashfs, e.config.rootfs);
  }

  // Configure rootfs.
  if (!e.lxc->set_config_item(
          e.lxc, "lxc.rootfs.path", e.config.rootfs.c_str())) {
    log_.error("Failed to set rootfs for '{}'", e.config.name);
    return false;
  }

  // Standard LXC identity and mounts.
  e.lxc->set_config_item(e.lxc, "lxc.uts.name", e.config.name.c_str());
  e.lxc->set_config_item(e.lxc, "lxc.mount.auto", "proc:mixed sys:mixed");
  e.lxc->set_config_item(e.lxc, "lxc.autodev", "1");
  e.lxc->set_config_item(e.lxc, "lxc.net.0.type", "empty");
  e.lxc->set_config_item(e.lxc, "lxc.pty.max", "1");

  const std::string log_file = "/tmp/lxc-" + e.config.name + ".log";
  e.lxc->set_config_item(e.lxc, "lxc.log.level", "TRACE");
  e.lxc->set_config_item(e.lxc, "lxc.log.file", log_file.c_str());

  log_.info("Container '{}' configured (log: {})", e.config.name, log_file);
  return true;
}

bool ContainerManager::Start(ContainerEntry& e) {
  if (!e.lxc) {
    log_.error("Start: no LXC handle for '{}'", e.config.name);
    return false;
  }

  log_.debug("Starting LXC container '{}'", e.config.name);

  std::vector<char*> argv;
  for (auto& a : e.config.args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  if (!argv.empty()) { argv.push_back(nullptr); }

  bool ok = e.lxc->start(e.lxc, 0, argv.empty() ? nullptr : argv.data());

  if (!ok) {
    log_.error("lxc start failed for '{}' — check /tmp/lxc-{}.log",
               e.config.name,
               e.config.name);
  } else {
    log_.info("Container '{}' started successfully", e.config.name);
  }

  return ok;
}

bool ContainerManager::Stop(ContainerEntry& e) {
  if (!e.lxc) return true;
  if (!e.lxc->is_running(e.lxc)) return true;

  log_.debug("Stopping LXC container '{}'", e.config.name);

  if (!e.lxc->shutdown(e.lxc, 5)) {
    log_.warn("Graceful shutdown failed for '{}' — forcing stop",
              e.config.name);
    return e.lxc->stop(e.lxc);
  }

  return true;
}

void ContainerManager::Release(ContainerEntry& e) {
  if (!e.lxc) return;

  log_.debug("Releasing LXC handle for '{}'", e.config.name);
  lxc_container_put(e.lxc);
  e.lxc = nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ContainerManager::SetState(ContainerEntry& e, ContainerState s) {
  if (e.state == s) return;

  const auto old = e.state;
  e.state = s;

  log_.info("Container '{}': {} -> {}",
            e.config.name,
            ContainerStateToString(old),
            ContainerStateToString(s));

  if (callback_) { callback_(e.config.name, old, s); }
}

ContainerManager::ContainerEntry* ContainerManager::Find(
    const std::string& name) {
  for (auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

const ContainerManager::ContainerEntry* ContainerManager::Find(
    const std::string& name) const {
  for (const auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

}  // namespace aember::container_manager
