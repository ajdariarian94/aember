/**
 * @file container_manager.cpp
 * @author Arian Ajdari
 * @brief Minimal ContainerManager implementation using LXC
 * @version 0.1
 * @date 2026-03-23
 */

#include <aember-libs/container-manager/container-manager.h>

#include <lxc/lxccontainer.h>

#include <sys/stat.h>
#include <unistd.h>

namespace aember::container_manager {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ContainerManager::ContainerManager(
    std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
    StateCallback cb)
    : mount_manager_(mount_manager),
      callback_(std::move(cb)),
      log_("container-manager") {
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

bool ContainerManager::AddContainer(
    const aember::utils::container::ContainerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config.name.empty() || config.rootfs.empty()) {
    log_.error("Invalid container config (name/rootfs required)");
    return false;
  }

  if (Find(config.name)) {
    log_.warn("Container '{}' already exists", config.name);
    return false;
  }

  aember::utils::container::ContainerEntry e;
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

  if (e->state == aember::utils::container::ContainerState::kRunning) {
    log_.warn("Container '{}' already running", name);
    return true;
  }

  log_.info("Starting container '{}'", name);
  SetState(*e, aember::utils::container::ContainerState::kStarting);

  if (!Create(*e)) {
    SetState(*e, aember::utils::container::ContainerState::kFailed);
    return false;
  }

  if (!Start(*e)) {
    SetState(*e, aember::utils::container::ContainerState::kFailed);
    return false;
  }

  SetState(*e, aember::utils::container::ContainerState::kRunning);
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
  SetState(*e, aember::utils::container::ContainerState::kStopping);

  bool ok = Stop(*e);

  Release(*e);
  SetState(*e,
           ok ? aember::utils::container::ContainerState::kStopped
              : aember::utils::container::ContainerState::kFailed);

  return ok;
}

aember::utils::container::ContainerState ContainerManager::GetContainerState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* e = Find(name);
  return e ? e->state : aember::utils::container::ContainerState::kFailed;
}

bool ContainerManager::IsRunning(const std::string& name) const {
  return GetContainerState(name) ==
         aember::utils::container::ContainerState::kRunning;
}

// ---------------------------------------------------------------------------
// LXC lifecycle
// ---------------------------------------------------------------------------

bool ContainerManager::Create(aember::utils::container::ContainerEntry& e) {
  log_.info("Container '{}' squashfs: '{}', rootfs: '{}'",
            e.config.name,
            e.config.squashfs,
            e.config.rootfs);
  Release(e);

  log_.debug("Creating container '{}'", e.config.name);

  // LXC handle
  const char* config_path = "/tmp";
  e.lxc = lxc_container_new(e.config.name.c_str(), config_path);
  if (!e.lxc) {
    log_.error("lxc_container_new failed for '{}'", e.config.name);
    return false;
  }

  // ----------------------------
  // Mount SquashFS if specified
  // ----------------------------
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

  // ----------------------------
  // Configure LXC rootfs
  // ----------------------------
  if (!e.lxc->set_config_item(
          e.lxc, "lxc.rootfs.path", e.config.rootfs.c_str())) {
    log_.error("Failed to set rootfs for '{}'", e.config.name);
    return false;
  }

  // LXC identity and mounts
  e.lxc->set_config_item(e.lxc, "lxc.uts.name", e.config.name.c_str());
  e.lxc->set_config_item(e.lxc, "lxc.mount.auto", "proc:mixed sys:mixed");
  e.lxc->set_config_item(e.lxc, "lxc.autodev", "1");
  e.lxc->set_config_item(e.lxc, "lxc.net.0.type", "empty");

  std::string log_file = "/tmp/lxc-" + e.config.name + ".log";
  e.lxc->set_config_item(e.lxc, "lxc.log.level", "TRACE");
  e.lxc->set_config_item(e.lxc, "lxc.log.file", log_file.c_str());
  e.lxc->set_config_item(e.lxc, "lxc.pty.max", "1");

  log_.info("Container '{}' configured (log: {})", e.config.name, log_file);

  return true;
}

bool ContainerManager::Start(aember::utils::container::ContainerEntry& e) {
  if (!e.lxc) {
    log_.error("Start: no LXC handle for '{}'", e.config.name);
    return false;
  }

  log_.debug("Starting LXC container '{}'", e.config.name);

  // Use args from config (fallback if empty)
  std::vector<char*> argv;
  if (!e.config.args.empty()) {
    for (auto& a : e.config.args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
  }

  bool ok = e.lxc->start(e.lxc, 0, argv.empty() ? nullptr : argv.data());

  if (!ok) {
    std::string log_file = "/tmp/lxc-" + e.config.name + ".log";
    log_.error("lxc start failed for '{}'", e.config.name);
    log_.error("Check logs: {}", log_file);
  } else {
    log_.info("Container '{}' started successfully", e.config.name);
  }

  return ok;
}

bool ContainerManager::Stop(aember::utils::container::ContainerEntry& e) {
  if (!e.lxc) return true;

  if (!e.lxc->is_running(e.lxc)) return true;

  log_.debug("Stopping LXC container '{}'", e.config.name);

  if (!e.lxc->shutdown(e.lxc, 5)) {
    log_.warn("Shutdown failed, forcing stop '{}'", e.config.name);
    return e.lxc->stop(e.lxc);
  }

  return true;
}

void ContainerManager::Release(aember::utils::container::ContainerEntry& e) {
  if (e.lxc) {
    log_.debug("Releasing container '{}'", e.config.name);
    lxc_container_put(e.lxc);
    e.lxc = nullptr;
  }
}

std::vector<ContainerManager::ContainerConfig> ContainerManager::LoadContainers(
    const std::string& name) {
  aember::utils::config::ConfigError error;

  if (!parser_.ParseFile(name, error)) {
    log_.error("Failed to load containers: {}", error.message);
    return {};
  }

  log_.info("Loaded container configuration from {}", name);

  return parser_.GetContainers();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ContainerManager::SetState(aember::utils::container::ContainerEntry& e,
                                aember::utils::container::ContainerState s) {
  if (e.state == s) return;

  auto old = e.state;
  e.state = s;

  log_.info("Container '{}' state: {} -> {}",
            e.config.name,
            ContainerStateToString(old),
            ContainerStateToString(s));

  if (callback_) { callback_(e.config.name, old, s); }
}

aember::utils::container::ContainerEntry* ContainerManager::Find(
    const std::string& name) {
  for (auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

const aember::utils::container::ContainerEntry* ContainerManager::Find(
    const std::string& name) const {
  for (auto& e : containers_) {
    if (e.config.name == name) return &e;
  }
  return nullptr;
}

}  // namespace aember::container_manager
