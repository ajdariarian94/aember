/**
 * @file container-manager.cpp
 * @author Arian Ajdari
 * @brief ContainerManager implementation using LXC with OverlayFS support.
 * @version 0.5
 * @date 2026-06-28
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/container-manager/container-manager.h>

#include <filesystem>
#include <format>
#include <fstream>

#include <lxc/lxccontainer.h>
#include <sys/mount.h>
#include <sys/prctl.h>
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

    RenameMonitorProcess(*e, name);
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

  // 1. Prepare and Mount OverlayFS over SquashFS
  if (!e.config.squashfs.empty()) {
    const std::string lower_dir = std::format("{}-lower", e.config.rootfs);
    const std::string overlay_base =
        std::format("/tmp/aember-overlay/{}", e.config.name);
    const std::string upper_dir = overlay_base + "/upper";
    const std::string work_dir = overlay_base + "/work/work";

    std::error_code ec;
    std::filesystem::create_directories(lower_dir, ec);
    std::filesystem::create_directories(upper_dir, ec);
    std::filesystem::create_directories(work_dir, ec);
    std::filesystem::create_directories(e.config.rootfs, ec);

    // Mount SquashFS to lowerdir
    if (!mount_manager_->MountSquashFS(e.config.squashfs, lower_dir)) {
      log_.error("Failed to mount squashfs '{}' at '{}'",
                 e.config.squashfs,
                 lower_dir);
      return false;
    }

    // Mount OverlayFS onto container rootfs
    const std::string overlay_opts = std::format(
        "lowerdir={},upperdir={},workdir={}", lower_dir, upper_dir, work_dir);

    if (mount("overlay",
              e.config.rootfs.c_str(),
              "overlay",
              0,
              overlay_opts.c_str()) != 0) {
      log_.error("Failed to mount OverlayFS at '{}': errno={} ({})",
                 e.config.rootfs,
                 errno,
                 strerror(errno));
      mount_manager_->Unmount(lower_dir);
      return false;
    }

    log_.info("Mounted OverlayFS over read-only SquashFS at '{}'",
              e.config.rootfs);

    // -------------------------------------------------------------------
    // Direct C++ Bind Mount: Guarantee host /var/log is attached inside
    // container rootfs BEFORE LXC starts up!
    // -------------------------------------------------------------------

    // 1. Ensure host /var/log exists before mounting
    std::filesystem::create_directories("/var/log", ec);

    // 2. Pre-create /host-logs inside container's merged rootfs
    const std::string container_host_logs = e.config.rootfs + "/host-logs";
    std::filesystem::create_directories(container_host_logs, ec);

    // 3. Perform raw Linux MS_BIND mount (host /var/log -> container
    // /host-logs)
    if (mount("/var/log",
              container_host_logs.c_str(),
              nullptr,
              MS_BIND,
              nullptr) != 0) {
      log_.error("Failed to bind mount /var/log onto '{}': errno={} ({})",
                 container_host_logs,
                 errno,
                 strerror(errno));
    } else {
      log_.info("Successfully bind-mounted host /var/log to '{}'",
                container_host_logs);
    }
  }

  // 2. Instantiate LXC handle AFTER OverlayFS and bind mounts are complete
  e.lxc =
      lxc_container_new(e.config.name.c_str(), "/var/lib/aember/containers");
  if (!e.lxc) {
    log_.error("lxc_container_new failed for '{}'", e.config.name);
    return false;
  }

  if (e.config.config_path.empty()) {
    log_.error("No config_path specified for container '{}'", e.config.name);
    return false;
  }

  // Load LXC config
  if (!e.lxc->load_config(e.lxc, e.config.config_path.c_str())) {
    log_.error("Failed to load config '{}' for '{}'",
               e.config.config_path,
               e.config.name);
    return false;
  }

  log_.info("Loaded container config from '{}'", e.config.config_path);
  return true;
}

bool ContainerManager::Start(ContainerEntry& e) {
  if (!e.lxc) {
    log_.error("Start: no LXC handle for '{}'", e.config.name);
    return false;
  }

  // Ensure host /var/log exists before starting LXC bind mounts
  std::error_code ec;
  std::filesystem::create_directories("/var/log", ec);

  log_.debug("Starting LXC container '{}'", e.config.name);

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
  if (e.lxc && e.lxc->is_running(e.lxc)) {
    log_.debug("Stopping LXC container '{}'", e.config.name);

    if (!e.lxc->shutdown(e.lxc, 5)) {
      log_.warn("Graceful shutdown failed for '{}' — forcing stop",
                e.config.name);
      e.lxc->stop(e.lxc);
    }
  }

  // Unmount OverlayFS first, then clean up SquashFS lowerdir
  if (!e.config.squashfs.empty()) {
    const std::string lower_dir = std::format("{}-lower", e.config.rootfs);

    // Unmount OverlayFS from rootfs
    umount2(e.config.rootfs.c_str(), MNT_DETACH);

    // Unmount SquashFS from lowerdir
    mount_manager_->Unmount(lower_dir);

    // Clean up temporary overlay work/upper directories
    std::error_code ec;
    std::filesystem::remove_all(
        std::format("/tmp/aember-overlay/{}", e.config.name), ec);
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
// Monitor process renaming
// ---------------------------------------------------------------------------

void ContainerManager::RenameMonitorProcess(ContainerEntry& e,
                                            std::string_view name) {
  pid_t monitor_pid = -1;

  char buf[32]{};
  if (e.lxc &&
      e.lxc->get_config_item(e.lxc, "lxc.monitor.pid", buf, sizeof(buf)) > 0) {
    try {
      monitor_pid = std::stoi(buf);
    } catch (...) {}
  }

  if (monitor_pid <= 0) { monitor_pid = FindMonitorPid(e); }

  if (monitor_pid <= 0) {
    log_.warn("Could not find monitor PID for '{}' — skipping rename", name);
    return;
  }

  const auto display_name =
      std::format("{{aember}} [lxc monitor] /tmp {}", name);
  const auto comm_path = std::format("/proc/{}/comm", monitor_pid);

  if (std::ofstream f{comm_path}; f.is_open()) {
    f << display_name;
    log_.debug("Renamed monitor PID {} to '{}'", monitor_pid, display_name);
  } else {
    log_.warn("Failed to write to {} for monitor rename: errno={} ({})",
              comm_path,
              errno,
              strerror(errno));
  }
}

pid_t ContainerManager::FindMonitorPid(const ContainerEntry& e) const {
  const pid_t our_pid = getpid();
  const pid_t init_pid = e.lxc ? e.lxc->init_pid(e.lxc) : -1;

  for (const auto& entry : std::filesystem::directory_iterator{
           "/proc",
           std::filesystem::directory_options::skip_permission_denied}) {
    if (!entry.is_directory()) continue;

    const auto name = entry.path().filename().string();
    if (!std::ranges::all_of(name, ::isdigit)) continue;

    pid_t pid = -1;
    try {
      pid = std::stoi(name);
    } catch (...) { continue; }

    if (pid == our_pid || pid == init_pid || pid <= 1) continue;

    std::ifstream status{std::format("/proc/{}/status", pid)};
    if (!status.is_open()) continue;

    std::string line;
    pid_t ppid = -1;
    long vm_rss = -1;

    while (std::getline(status, line)) {
      if (line.starts_with("PPid:")) {
        try {
          ppid = std::stoi(line.substr(5));
        } catch (...) {}
      } else if (line.starts_with("VmRSS:")) {
        try {
          vm_rss = std::stol(line.substr(6));
        } catch (...) {}
      }
    }

    if (ppid != our_pid) continue;

    if (vm_rss > 0 && vm_rss < 30000) {
      log_.debug(
          "Found monitor PID {} (PPid={} VmRSS={}kB)", pid, ppid, vm_rss);
      return pid;
    }
  }

  return -1;
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
