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
#include <aember-libs/container-manager/lxc-api.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>

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
  const auto& containers = parser_.GetContainers();
  log_.info("Parser returned {} containers:", containers.size());
  for (const auto& c : containers) { log_.info("  - {}", c.name); }
  return containers;
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

  // Settings for every container from the environment aember runs in (e.g.
  // the chroot launcher passes the X11 display through). Optional file.
  if (std::ifstream extra{extra_config_path_}; extra.is_open()) {
    std::stringstream text;
    text << extra.rdbuf();
    for (const auto& [key, value] : ParseExtraConfig(text.str())) {
      if (!e.lxc->set_config_item(e.lxc, key.c_str(), value.c_str())) {
        log_.warn(
            "'{}': extra config {} = {} rejected", e.config.name, key, value);
      } else {
        log_.debug("'{}': extra config {} = {}", e.config.name, key, value);
      }
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::string>>
ContainerManager::ParseExtraConfig(std::string_view text) {
  // "key = value" per line, like lxc.container.conf; '#' starts a comment.
  const auto trim = [](std::string_view v) {
    const auto b = v.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return std::string_view{};
    const auto e = v.find_last_not_of(" \t\r");
    return v.substr(b, e - b + 1);
  };
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto line_range : std::views::split(text, '\n')) {
    const std::string_view line{line_range.begin(), line_range.end()};
    const auto content = trim(line);
    if (content.empty() || content.front() == '#') continue;
    const auto eq = content.find('=');
    if (eq == std::string_view::npos) continue;
    const auto key = trim(content.substr(0, eq));
    const auto value = trim(content.substr(eq + 1));
    if (key.empty()) continue;
    items.emplace_back(std::string{key}, std::string{value});
  }
  return items;
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

}  // namespace aember::container_manager
