/**
 * @file container_manager.cpp
 * @author Arian Ajdari
 * @brief Library implementation for ContainerManager.
 *        Manages LXC container lifecycle via liblxc directly.
 * @version 0.1
 * @date 2025-07-18
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/container-manager/container-manager.h>

#include <lxc/lxccontainer.h>

#include <chrono>

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

ContainerManager::ContainerManager(const nlohmann::json& config,
                                   StateCallback cb)
    : callback_(std::move(cb)), log_("container-manager") {
  log_.info("Initializing ContainerManager");

  ParseConfig(config);

  log_.info("Loaded {} container(s)", containers_.size());
}

ContainerManager::~ContainerManager() {
  Stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ContainerManager::Start() {
  log_.info("Starting ContainerManager");

  running_ = true;

  for (auto& e : containers_) {
    if (e.config.auto_start) {
      log_.info("Auto-starting container '{}'", e.config.name);
      StartContainer(e.config.name);
    }
  }

  monitor_thread_ = std::thread(&ContainerManager::MonitorLoop, this);

  log_.info("ContainerManager started");
}

void ContainerManager::Stop() {
  log_.info("Stopping ContainerManager");

  running_ = false;
  cv_.notify_all();

  if (monitor_thread_.joinable()) { monitor_thread_.join(); }

  std::lock_guard<std::mutex> lock(containers_mutex_);

  for (auto& e : containers_) {
    if (e.state == ContainerState::kRunning) {
      log_.info("Stopping container '{}'", e.config.name);
    }

    Stop(e);
    Release(e);
  }

  log_.info("ContainerManager stopped");
}

bool ContainerManager::StartContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(containers_mutex_);

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
    log_.error("Create failed for '{}'", name);
    SetState(*e, ContainerState::kFailed);
    return false;
  }

  if (!Start(*e)) {
    log_.error("Start failed for '{}'", name);
    SetState(*e, ContainerState::kFailed);
    return false;
  }

  e->restart_count = 0;
  e->started_at_ms = NowMs();

  SetState(*e, ContainerState::kRunning);

  log_.info("Container '{}' is now running", name);
  return true;
}

bool ContainerManager::StopContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(containers_mutex_);

  auto* e = Find(name);
  if (!e) {
    log_.error("StopContainer: '{}' not found", name);
    return false;
  }

  if (e->state != ContainerState::kRunning) {
    log_.warn("Container '{}' is not running", name);
  }

  log_.info("Stopping container '{}'", name);

  SetState(*e, ContainerState::kStopping);

  bool ok = Stop(*e);

  if (!ok) { log_.error("Failed to stop container '{}'", name); }

  Release(*e);

  e->stopped_at_ms = NowMs();
  SetState(*e, ok ? ContainerState::kStopped : ContainerState::kFailed);

  return ok;
}

bool ContainerManager::DestroyContainer(const std::string& name) {
  std::lock_guard<std::mutex> lock(containers_mutex_);

  auto* e = Find(name);
  if (!e) {
    log_.error("DestroyContainer: '{}' not found", name);
    return false;
  }

  log_.info("Destroying container '{}'", name);

  Stop(*e);

  if (e->lxc) {
    if (!e->lxc->destroy(e->lxc)) {
      log_.error("Failed to destroy container '{}'", name);
    }
  }

  Release(*e);
  SetState(*e, ContainerState::kStopped);

  return true;
}

ContainerState ContainerManager::GetContainerState(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(containers_mutex_);

  auto* e = Find(name);
  if (!e) { return ContainerState::kFailed; }

  return e->state;
}

bool ContainerManager::IsRunning(const std::string& name) const {
  return GetContainerState(name) == ContainerState::kRunning;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void ContainerManager::ParseConfig(const nlohmann::json& config) {
  if (config.contains("monitor_interval_ms")) {
    interval_ =
        std::chrono::milliseconds(config["monitor_interval_ms"].get<int>());
  }

  if (!config.contains("containers")) {
    log_.warn("No containers defined in config");
    return;
  }

  for (auto& j : config["containers"]) {
    ContainerEntry e;

    e.config.name = j.at("name").get<std::string>();
    e.config.rootfs = j.at("rootfs").get<std::string>();

    e.config.auto_start = j.value("auto_start", true);
    e.config.restart_on_crash = j.value("restart_on_crash", true);
    e.config.restart_delay_ms = j.value("restart_delay_ms", 2000);
    e.config.max_restarts = j.value("max_restarts", 5);

    if (j.contains("args")) {
      for (auto& a : j["args"]) {
        e.config.args.push_back(a.get<std::string>());
      }
    }

    log_.info("Configured container '{}' (rootfs={})",
              e.config.name,
              e.config.rootfs);

    containers_.push_back(std::move(e));
  }
}

// ---------------------------------------------------------------------------
// LXC
// ---------------------------------------------------------------------------

bool ContainerManager::Create(ContainerEntry& e) {
  Release(e);

  log_.debug("Creating container '{}'", e.config.name);

  e.lxc = lxc_container_new(e.config.name.c_str(), e.config.lxc_path.c_str());

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

  log_.debug("Starting LXC container '{}'", e.config.name);

  std::vector<char*> argv;
  for (auto& a : e.config.args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  bool ok = e.lxc->start(e.lxc, 1, argv.size() > 1 ? argv.data() : nullptr);

  if (!ok) { log_.error("lxc start failed for '{}'", e.config.name); }

  return ok;
}

bool ContainerManager::Stop(ContainerEntry& e) {
  if (!e.lxc) return true;

  if (!e.lxc->is_running(e.lxc)) return true;

  log_.debug("Shutting down container '{}'", e.config.name);

  if (!e.lxc->shutdown(e.lxc, 5)) {
    log_.warn("Graceful shutdown failed for '{}', forcing stop", e.config.name);
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
// Monitor
// ---------------------------------------------------------------------------

void ContainerManager::MonitorLoop() {
  log_.info("Container monitor started (interval={} ms)", interval_.count());

  std::unique_lock<std::mutex> lock(cv_mutex_);

  while (running_) {
    lock.unlock();

    {
      std::lock_guard<std::mutex> g(containers_mutex_);
      for (auto& e : containers_) { Tick(e); }
    }

    lock.lock();
    cv_.wait_for(lock, interval_, [this] { return !running_; });
  }

  log_.info("Container monitor stopped");
}

void ContainerManager::Tick(ContainerEntry& e) {
  if (!e.lxc) return;

  if (e.state == ContainerState::kRunning && e.lxc->is_running(e.lxc)) {
    return;
  }

  if (e.state == ContainerState::kRunning && !e.lxc->is_running(e.lxc)) {
    log_.warn("Container '{}' crashed", e.config.name);

    SetState(e, ContainerState::kStopped);
    e.stopped_at_ms = NowMs();

    if (!e.config.restart_on_crash) {
      log_.info("Restart disabled for '{}'", e.config.name);
      return;
    }

    if (e.config.max_restarts >= 0 &&
        e.restart_count >= e.config.max_restarts) {
      log_.error("Container '{}' exceeded max restarts", e.config.name);
      SetState(e, ContainerState::kFailed);
      return;
    }

    e.next_restart_at_ms = NowMs() + e.config.restart_delay_ms;

    log_.info("Scheduling restart for '{}' in {} ms",
              e.config.name,
              e.config.restart_delay_ms);
  }

  if (e.state == ContainerState::kStopped && e.next_restart_at_ms > 0 &&
      NowMs() >= e.next_restart_at_ms) {
    log_.info("Restarting container '{}' (attempt {})",
              e.config.name,
              e.restart_count + 1);

    e.next_restart_at_ms = 0;

    if (Create(e) && Start(e)) {
      e.started_at_ms = NowMs();
      e.restart_count++;

      SetState(e, ContainerState::kRunning);

      log_.info("Container '{}' restarted successfully", e.config.name);
    } else {
      log_.error("Restart failed for '{}'", e.config.name);
      SetState(e, ContainerState::kFailed);
    }
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

int64_t ContainerManager::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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
