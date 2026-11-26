/**
 * @file container-manager.cpp
 * @author Arian Ajdari
 * @brief ContainerManager implementation using LXC with OverlayFS support.
 * @version 0.5
 * @date 2026-06-28
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-services/containerd/container-manager.h>
#include <aember-services/containerd/init-wrapper.h>
#include <aember-services/containerd/lxc-api.h>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <lxc/lxccontainer.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aember::containerd {

namespace {

/// A file that is (or was, in a reused rootfs) bind mounted: not empty.
bool HasInitWrapper(const std::string& rootfs) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(rootfs + kInitInContainer, ec);
  return !ec && size > 0;
}

/// umount2(path, 0); if it is busy, detach it lazily. False only if it
/// couldn't be unmounted at all (not mounted counts as done).
bool UnmountPath(const std::string& path, aember::utils::logging::Logger& log) {
  if (::umount2(path.c_str(), 0) == 0) {
    log.info("Unmounted {}", path);
    return true;
  }
  if (errno == EINVAL || errno == ENOENT) {
    log.info("{} was not mounted", path);
    return true;
  }
  const int err = errno;
  if (::umount2(path.c_str(), MNT_DETACH) == 0) {
    log.warn("{} busy ({}), detached lazily",
             path,
             std::system_category().message(err));
    return true;
  }
  log.warn(
      "Cannot unmount {}: {}", path, std::system_category().message(errno));
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

ContainerManager::ContainerManager(
    std::shared_ptr<aember::mount_manager::MountManager> mount_manager,
    StateCallback cb)
    : callback_(std::move(cb)), mount_manager_(std::move(mount_manager)) {
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

bool ContainerManager::UpdateConfig(const ContainerConfig& config) {
  std::lock_guard lock{mutex_};
  auto* e = Find(config.name);
  if (!e || (e->state != ContainerState::kStopped &&
             e->state != ContainerState::kFailed)) {
    return false;
  }
  e->config = config;
  e->restarts = 0;
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
  ContainerEntry* e = nullptr;
  {
    std::lock_guard lock{mutex_};
    e = Find(name);
    if (!e) {
      log_.error("StartContainer: '{}' not found", name);
      return false;
    }
    if (e->state == ContainerState::kRunning ||
        e->state == ContainerState::kStarting) {
      log_.warn(
          "Container '{}' already {}", name, ContainerStateToString(e->state));
      return true;
    }
    log_.info("Starting container '{}'", name);
    SetState(*e, ContainerState::kStarting);
  }

  // Without the lock: mounting and starting take a while, and other
  // containers start meanwhile. Only this entry is touched (the entries
  // never move: the list is fixed before anything starts), and nothing else
  // acts on a container that is starting.
  const bool ok = Create(*e) && Start(*e);

  std::lock_guard lock{mutex_};
  if (!ok) {
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
  e->stop_requested = false;
  e->started_at = std::chrono::steady_clock::now();
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
  e->stop_requested = true;
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
  if (std::chrono::steady_clock::now() - e->started_at >= kStableRun) {
    e->restarts = 0;  // it ran fine for a while: a fresh failure
  }
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

std::optional<std::chrono::seconds> ContainerManager::NextRestart(
    std::string_view name) {
  std::lock_guard lock{mutex_};
  auto* e = Find(name);
  if (!e || e->stop_requested) return std::nullopt;
  const auto& cfg = e->config;
  switch (cfg.restart_policy) {
    case RestartPolicy::kNo:
      return std::nullopt;
    case RestartPolicy::kOnFailure:
      if (e->state != ContainerState::kFailed) return std::nullopt;
      break;
    case RestartPolicy::kAlways:
      if (e->state == ContainerState::kRunning ||
          e->state == ContainerState::kStarting) {
        return std::nullopt;
      }
      break;
  }
  if (cfg.max_restart_attempts > 0 && e->restarts >= cfg.max_restart_attempts) {
    log_.error(
        "Container '{}': {} restart(s) in a row, giving up", name, e->restarts);
    return std::nullopt;
  }
  ++e->restarts;
  log_.info("Container '{}': restart {}{} in {} s ({})",
            name,
            e->restarts,
            cfg.max_restart_attempts > 0
                ? "/" + std::to_string(cfg.max_restart_attempts)
                : std::string{},
            cfg.restart_delay.count(),
            ToString(cfg.restart_policy));
  return cfg.restart_delay;
}

// ---------------------------------------------------------------------------
// Adopt / StopAll (containerd)
// ---------------------------------------------------------------------------

bool ContainerManager::Adopt(std::string_view name) {
  std::lock_guard lock{mutex_};
  auto* e = Find(name);
  const auto* lxc = Lxc();
  if (e == nullptr || lxc == nullptr || e->lxc != nullptr) { return false; }

  lxc_container* c = lxc->container_new(e->config.name.c_str(), kLxcPath);
  if (c == nullptr) { return false; }
  if (!c->is_running(c)) {
    lxc->container_put(c);
    return false;
  }

  e->lxc = c;
  if (const pid_t pid = c->init_pid(c); pid > 0) {
    pid_to_name_[pid] = e->config.name;
  }
  log_.info("Adopted running container '{}'", name);
  SetState(*e, ContainerState::kRunning);
  return true;
}

void ContainerManager::StopAll() {
  std::vector<std::string> names;
  {
    std::lock_guard lock{mutex_};
    for (const auto& e : containers_) {
      if (e.state == ContainerState::kRunning ||
          e.state == ContainerState::kStarting) {
        names.push_back(e.config.name);
      }
    }
  }
  for (const auto& name : names) { StopContainer(name); }
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
  // Only while it runs: a starting container's handle is being set up by
  // the thread that starts it.
  if (!e || !e->lxc || e->state != ContainerState::kRunning) {
    return std::nullopt;
  }
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

// ---------------------------------------------------------------------------
// LXC lifecycle helpers
// ---------------------------------------------------------------------------

bool ContainerManager::Create(ContainerEntry& e) {
  Release(e);

  log_.debug("Creating container '{}' (squashfs='{}', rootfs='{}')",
             e.config.name,
             e.config.squashfs,
             e.config.rootfs);

  // 1. Prepare and Mount OverlayFS over SquashFS. A container that died
  // while containerd was down still has its mounts: reuse them rather than
  // stacking a second overlay on the same upper/work dirs.
  if (!e.config.squashfs.empty() &&
      mount_manager_->IsMounted(e.config.rootfs)) {
    log_.info("Reusing the existing rootfs mount of '{}'", e.config.name);
  } else if (!e.config.squashfs.empty()) {
    const std::string lower_dir = fmt::format("{}-lower", e.config.rootfs);
    const std::string overlay_base =
        fmt::format("/tmp/aember-overlay/{}", e.config.name);
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
    const std::string overlay_opts = fmt::format(
        "lowerdir={},upperdir={},workdir={}", lower_dir, upper_dir, work_dir);

    if (mount("overlay",
              e.config.rootfs.c_str(),
              "overlay",
              0,
              overlay_opts.c_str()) != 0) {
      log_.error("Failed to mount OverlayFS at '{}': errno={} ({})",
                 e.config.rootfs,
                 errno,
                 std::system_category().message(errno));
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
    // /host-logs). Recursive: PID 1's status file is a bind mount of a file
    // on /run (tmpfs) over /var/log/aember-status.json.
    if (mount("/var/log",
              container_host_logs.c_str(),
              nullptr,
              MS_BIND | MS_REC,
              nullptr) != 0) {
      log_.error("Failed to bind mount /var/log onto '{}': errno={} ({})",
                 container_host_logs,
                 errno,
                 std::system_category().message(errno));
    } else {
      log_.info("Successfully bind-mounted host /var/log to '{}'",
                container_host_logs);
    }

    // 4. The init wrapper: the host's static BusyBox as /busybox.aember-init.
    if (e.config.init) {
      const std::string target = e.config.rootfs + kInitInContainer;
      { std::ofstream{target, std::ios::app}; }  // bind target must exist
      if (mount(host_busybox_.c_str(),
                target.c_str(),
                nullptr,
                MS_BIND | MS_RDONLY,
                nullptr) != 0) {
        log_.warn("'{}': no init wrapper (bind {}: {}); the app is init",
                  e.config.name,
                  host_busybox_,
                  std::system_category().message(errno));
      }
    }
  }
  e.init_wrapper = e.config.init && HasInitWrapper(e.config.rootfs);

  // 2. Instantiate LXC handle AFTER OverlayFS and bind mounts are complete
  const auto* lxc = Lxc();
  if (lxc == nullptr) {
    log_.error("liblxc not available: {}", LxcLoadError());
    return false;
  }
  e.lxc = lxc->container_new(e.config.name.c_str(), kLxcPath);
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

  // `lxc shutdown` sends lxc.signal.halt (SIGPWR by default) to the init:
  // the wrapper passes SIGTERM on to the app.
  if (e.init_wrapper &&
      !e.lxc->set_config_item(e.lxc, "lxc.signal.halt", "SIGTERM")) {
    log_.warn("'{}': lxc.signal.halt = SIGTERM rejected", e.config.name);
  }
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

  std::vector<std::string> args;
  if (e.init_wrapper && !e.config.args.empty()) {
    args = {kInitInContainer, "sh", "-c", kInitScript, "aember-init"};
    log_.debug("'{}' runs under the init wrapper", e.config.name);
  }
  args.insert(args.end(), e.config.args.begin(), e.config.args.end());

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  std::ranges::transform(args, std::back_inserter(argv), [](auto& a) {
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
  // False only if the container may still be running (StopContainer then
  // reports it as failed, not stopped). The mounts are released either way.
  bool stopped = true;
  if (e.lxc && e.lxc->is_running(e.lxc)) {
    log_.debug("Stopping LXC container '{}'", e.config.name);

    const auto timeout = static_cast<int>(e.config.stop_timeout.count());
    if (!e.lxc->shutdown(e.lxc, timeout)) {
      log_.warn("Graceful shutdown failed for '{}' — forcing stop",
                e.config.name);
      if (!e.lxc->stop(e.lxc)) {
        log_.error("Container '{}' could not be stopped", e.config.name);
        stopped = false;
      }
    }
  }

  // Innermost first: the bind mounts inside the rootfs (they kept the
  // overlay busy, so it was only detached lazily and the SquashFS under it
  // stayed in use), then the overlay, then the SquashFS lowerdir.
  if (!e.config.squashfs.empty()) {
    const std::string lower_dir = fmt::format("{}-lower", e.config.rootfs);

    UnmountPath(e.config.rootfs + kInitInContainer, log_);
    UnmountPath(e.config.rootfs + "/host-logs/aember-status.json", log_);
    UnmountPath(e.config.rootfs + "/host-logs", log_);
    UnmountPath(e.config.rootfs, log_);
    e.init_wrapper = false;

    // The SquashFS lowerdir can stay busy after the overlay is gone (the
    // overlay superblock outlives its mount for a while; see TODO.md). Try
    // for 1 s, then detach it lazily: the loop device clears itself
    // (autoclear) once the last user lets go.
    bool unmounted = false;
    for (int i = 0; i < 10 && !unmounted; ++i) {
      unmounted = ::umount2(lower_dir.c_str(), 0) == 0 || errno == EINVAL;
      if (!unmounted && errno != EBUSY) break;
      if (!unmounted) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    if (!unmounted && ::umount2(lower_dir.c_str(), MNT_DETACH) == 0) {
      log_.warn("{} still busy, detached lazily", lower_dir);
    }

    // Unmount SquashFS from lowerdir
    mount_manager_->Unmount(lower_dir);

    // Clean up temporary overlay work/upper directories
    std::error_code ec;
    std::filesystem::remove_all(
        fmt::format("/tmp/aember-overlay/{}", e.config.name), ec);
  }

  return stopped;
}

void ContainerManager::Release(ContainerEntry& e) {
  if (!e.lxc) return;
  log_.debug("Releasing LXC handle for '{}'", e.config.name);
  if (const auto* lxc = Lxc()) { lxc->container_put(e.lxc); }
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

}  // namespace aember::containerd
