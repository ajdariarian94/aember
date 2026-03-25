/**
 * @file container_manager.cpp
 * @author Arian Ajdari
 * @brief Minimal ContainerManager implementation using LXC
 * @version 0.1
 * @date 2026-03-23
 */

#include <aember-libs/container-manager/container-manager.h>

#include <lxc/lxccontainer.h>

namespace aember::container_manager {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string ContainerStateToString(ContainerState s) {
  switch (s) {
    case ContainerState::kStopped:
      return "stopped";
    case ContainerState::kStarting:
      return "starting";
    case ContainerState::kRunning:
      return "running";
    case ContainerState::kStopping:
      return "stopping";
    case ContainerState::kFailed:
      return "failed";
    default:
      return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ContainerManager::ContainerManager(StateCallback cb)
    : callback_(std::move(cb)), log_("container-manager") {
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
// Public API
// ---------------------------------------------------------------------------

bool ContainerManager::AddContainer(const ContainerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty() || config.rootfs.empty()) {
    log_.error("Invalid container config (name/rootfs required)");
    return false;
  }

  if (Find(config.name)) {
    log_.warn("Container '{}' already exists", config.name);
    return false;
  }

  ContainerEntry e;
  e.config = config;

  containers_.push_back(std::move(e));

  log_.info("Added container '{}' (rootfs={})", config.name, config.rootfs);

  return true;
}

bool ContainerManager::RemoveContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = containers_.begin(); it != containers_.end(); ++it) {
    if (it->config.name == name) {
      Stop(*it);
      Release(*it);
      containers_.erase(it);

      log_.info("Removed container '{}'", name);
      return true;
    }
  }

  log_.warn("RemoveContainer: '{}' not found", name);
  return false;
}

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

  log_.info("Stopping container '{}'", name);
  SetState(*e, ContainerState::kStopping);

  bool ok = Stop(*e);

  Release(*e);
  SetState(*e, ok ? ContainerState::kStopped : ContainerState::kFailed);

  return ok;
}

ContainerState ContainerManager::GetContainerState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* e = Find(name);
  return e ? e->state : ContainerState::kFailed;
}

bool ContainerManager::IsRunning(const std::string& name) const {
  return GetContainerState(name) == ContainerState::kRunning;
}

// ---------------------------------------------------------------------------
// LXC lifecycle
// ---------------------------------------------------------------------------

bool ContainerManager::Create(ContainerEntry& e) {
  Release(e);

  log_.debug("Creating container '{}'", e.config.name);

  e.lxc = lxc_container_new(e.config.name.c_str(), nullptr);
  if (!e.lxc) {
    log_.error("lxc_container_new failed for '{}'", e.config.name);
    return false;
  }

  if (!e.lxc->set_config_item(
          e.lxc, "lxc.rootfs.path", e.config.rootfs.c_str())) {
    log_.error("Failed to set rootfs for '{}'", e.config.name);
    return false;
  }

  return true;
}

bool ContainerManager::Start(ContainerEntry& e) {
  if (!e.lxc) {
    log_.error("Start: no LXC handle for '{}'", e.config.name);
    return false;
  }

  std::vector<char*> argv;
  for (auto& a : e.config.args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  log_.debug("Starting LXC container '{}'", e.config.name);

  bool ok = e.lxc->start(e.lxc, 1, argv.size() > 1 ? argv.data() : nullptr);

  if (!ok) { log_.error("lxc start failed for '{}'", e.config.name); }

  return ok;
}

bool ContainerManager::Stop(ContainerEntry& e) {
  if (!e.lxc) return true;

  if (!e.lxc->is_running(e.lxc)) return true;

  log_.debug("Stopping LXC container '{}'", e.config.name);

  if (!e.lxc->shutdown(e.lxc, 5)) {
    log_.warn("Shutdown failed, forcing stop '{}'", e.config.name);
    return e.lxc->stop(e.lxc);
  }

  return true;
}

void ContainerManager::Release(ContainerEntry& e) {
  if (e.lxc) {
    log_.debug("Releasing container '{}'", e.config.name);
    lxc_container_put(e.lxc);
    e.lxc = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ContainerManager::SetState(ContainerEntry& e, ContainerState s) {
  if (e.state == s) return;

  auto old = e.state;
  e.state = s;

  log_.info("Container '{}' state: {} -> {}",
            e.config.name,
            ContainerStateToString(old),
            ContainerStateToString(s));

  if (callback_) { callback_(e.config.name, old, s); }
}

ContainerEntry* ContainerManager::Find(const std::string& name) {
  for (auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

const ContainerEntry* ContainerManager::Find(const std::string& name) const {
  for (auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

}  // namespace aember::container_manager
