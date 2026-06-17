/**
 * @file container-manager.cpp
 * @author Arian Ajdari
 * @brief ContainerManager implementation using LXC.
 * @version 0.3
 * @date 2026-06-12
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/container-manager/container-manager.h>

#include <lxc/lxccontainer.h>

#include <sys/stat.h>
#include <unistd.h>

#include <format>
#include <ranges>

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
  std::lock_guard lock{mutex_};
  for (auto& e : containers_) {
    Stop(e);
    Release(e);
  }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

bool ContainerManager::AddContainer(const ContainerConfig& config) {
  std::lock_guard lock{mutex_};

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

bool ContainerManager::RemoveContainer(std::string_view name) {
  std::lock_guard lock{mutex_};

  auto it = std::ranges::find_if(containers_, [name](const ContainerEntry& e) {
    return e.config.name == name;
  });

  if (it == containers_.end()) {
    log_.warn("RemoveContainer: '{}' not found", name);
    return false;
  }

  if (it->lxc && it->lxc->is_running(it->lxc)) {
    if (const pid_t pid = it->lxc->init_pid(it->lxc); pid > 0) {
      pid_to_name_.erase(pid);
    }
  }

  Stop(*it);
  Release(*it);
  containers_.erase(it);

  log_.info("Removed container '{}'", name);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ContainerManager::StartContainer(std::string_view name) {
  std::lock_guard lock{mutex_};

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

  if (e->lxc) {
    if (const pid_t pid = e->lxc->init_pid(e->lxc); pid > 0) {
      pid_to_name_[pid] = std::string{name};
      log_.debug("Container '{}' init PID: {}", name, pid);
    } else {
      log_.warn("Container '{}' started but init_pid() returned {}", name, pid);
    }
  }

  SetState(*e, ContainerState::kRunning);
  return true;
}

bool ContainerManager::StopContainer(std::string_view name) {
  std::lock_guard lock{mutex_};

  auto* e = Find(name);
  if (!e) {
    log_.error("StopContainer: '{}' not found", name);
    return false;
  }

  if (e->state == ContainerState::kStopped ||
      e->state == ContainerState::kStopping) {
    return true;
  }

  if (e->lxc) {
    if (const pid_t pid = e->lxc->init_pid(e->lxc); pid > 0) {
      pid_to_name_.erase(pid);
    }
  }

  log_.info("Stopping container '{}'", name);
  SetState(*e, ContainerState::kStopping);

  const bool ok = Stop(*e);
  Release(*e);

  SetState(*e, ok ? ContainerState::kStopped : ContainerState::kFailed);
  return ok;
}

// ---------------------------------------------------------------------------
// SIGCHLD integration
// ---------------------------------------------------------------------------

bool ContainerManager::HandleExit(pid_t pid, int exit_code) {
  std::lock_guard lock{mutex_};

  auto it = pid_to_name_.find(pid);
  if (it == pid_to_name_.end()) { return false; }

  const std::string name = std::move(it->second);
  pid_to_name_.erase(it);

  auto* e = Find(name);
  if (!e) { return true; }

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

bool ContainerManager::HasContainer(std::string_view name) const {
  std::lock_guard lock{mutex_};
  return Find(name) != nullptr;
}

ContainerManager::ContainerState ContainerManager::GetContainerState(
    std::string_view name) const {
  std::lock_guard lock{mutex_};
  const auto* e = Find(name);
  return e ? e->state : ContainerState::kFailed;
}

bool ContainerManager::IsRunning(std::string_view name) const {
  return GetContainerState(name) == ContainerState::kRunning;
}

std::optional<pid_t> ContainerManager::GetInitPid(std::string_view name) const {
  std::lock_guard lock{mutex_};
  const auto* e = Find(name);
  if (!e || !e->lxc) return std::nullopt;
  const pid_t pid = e->lxc->init_pid(e->lxc);
  return pid > 0 ? std::optional<pid_t>{pid} : std::nullopt;
}

// ---------------------------------------------------------------------------
// Config loading + callback
// ---------------------------------------------------------------------------

void ContainerManager::SetStateCallback(StateCallback callback) {
  std::lock_guard lock{mutex_};
  callback_ = std::move(callback);
}

std::vector<ContainerManager::ContainerConfig> ContainerManager::LoadContainers(
    std::string_view source) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(std::string{source}, error)) {
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

  if (e.config.config_path.empty()) {
    log_.error("No config_path specified for container '{}' — cannot start",
               e.config.name);
    return false;
  }

  if (!e.lxc->load_config(e.lxc, e.config.config_path.c_str())) {
    log_.error("Failed to load config '{}' for '{}'",
               e.config.config_path,
               e.config.name);
    return false;
  }

  log_.info("Loaded container config from '{}'", e.config.config_path);

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

  log_.info("Container '{}' configured", e.config.name);
  return true;
}

bool ContainerManager::Start(ContainerEntry& e) {
  if (!e.lxc) {
    log_.error("Start: no LXC handle for '{}'", e.config.name);
    return false;
  }

  log_.debug("Starting LXC container '{}'", e.config.name);

  // LXC C API requires char* argv — const_cast is unavoidable here.
  std::vector<char*> argv;
  argv.reserve(e.config.args.size() + 1);
  std::ranges::transform(e.config.args, std::back_inserter(argv), [](auto& a) {
    return const_cast<char*>(a.c_str());
  });
  if (!argv.empty()) { argv.push_back(nullptr); }

  const bool ok = e.lxc->start(e.lxc, 0, argv.empty() ? nullptr : argv.data());

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
  if (!e.lxc || !e.lxc->is_running(e.lxc)) return true;

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
    std::string_view name) {
  auto it = std::ranges::find_if(containers_, [name](const ContainerEntry& e) {
    return e.config.name == name;
  });
  return it != containers_.end() ? &*it : nullptr;
}

const ContainerManager::ContainerEntry* ContainerManager::Find(
    std::string_view name) const {
  auto it = std::ranges::find_if(containers_, [name](const ContainerEntry& e) {
    return e.config.name == name;
  });
  return it != containers_.end() ? &*it : nullptr;
}

}  // namespace aember::container_manager
