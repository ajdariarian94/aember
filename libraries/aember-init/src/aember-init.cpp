/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of the AemberInit init system.
 */

#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/device-health/aember-monitor.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <sys/wait.h>

namespace aember::aember_init {

namespace {

/// Run one init phase. PID 1 must never die, so an exception from a phase is
/// logged and init carries on with the next one.
template <typename F>
bool Guarded(aember::utils::logging::Logger& log, std::string_view phase,
             F&& fn) {
  try {
    std::forward<F>(fn)();
    return true;
  } catch (const std::exception& e) {
    log.error("{} failed: {}", phase, e.what());
  } catch (...) { log.error("{} failed: unknown exception", phase); }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

AemberInit::AemberInit(std::string_view logger_name, Options options)
    : options_(options), log_(std::string{logger_name}) {
#ifndef NDEBUG
  aember::utils::logging::logging_level_debug();
#else
  aember::utils::logging::logging_level_info();
#endif
}

AemberInit::~AemberInit() {
  Stop();
}

// ---------------------------------------------------------------------------
// StartInitramfs
// ---------------------------------------------------------------------------

void AemberInit::StartInitramfs() {
  if (running_.exchange(true)) return;

  log_.info("Starting Aember in initramfs mode");

  Guarded(log_, "Initramfs mounts", [this] {
    mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();
    mount_manager_->MountInitramfsFilesystems();
    if (!mount_manager_->MountEarlyFilesystems()) {
      log_.warn("Some early filesystems failed to mount, continuing anyway");
    }
  });

  log_.info("Initramfs detected, preparing to pivot to real root");

  // PerformPivot exec's switch_root on success and does not return.
  Guarded(log_, "Pivot to real root", [this] {
    root_manager_ =
        std::make_unique<aember::root_manager::RootManager>(mount_manager_);

    aember::utils::root::RootConfig root_config;
    root_config.ParseFromProcCmdline();

    if (!mount_manager_->EnsureDirectory(root_config.new_root_path)) {
      log_.error("Failed to create pivot mount point: {}",
                 root_config.new_root_path);
    }

    if (!root_manager_->PerformPivot(root_config)) {
      log_.warn("Pivot to real root failed");
    }
  });

  log_.warn("Continuing in initramfs");

  Guarded(log_, "Signal init", [this] { std::ignore = InitSignals(); });
  StartReaperThread();

  // Emergency shell: the real root is unusable, so open one right away.
  StartDebugShell(/*shell_now=*/true);

  Guarded(log_, "Heartbeat", [this] {
    heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
    heartbeat_->Start();
  });

  log_.info("Aember Init System started successfully");
  RunLoop();
  Stop();
}

// ---------------------------------------------------------------------------
// StartRoot
// ---------------------------------------------------------------------------

void AemberInit::StartRoot() {
  if (running_.exchange(true)) return;

  aember::utils::logging::enable_file_logging("/var/log/aember-init.log");
  log_.info("Starting Aember in root mode{}",
            options_.chroot ? " (chroot)" : "");

  // Create heartbeat JSON file early — needed for aember-monitor container
  // mount
  {
    std::ofstream file{"/var/log/aember-monitor.json"};
    file << "{}";
  }

  Guarded(log_, "Mount phase", [this] {
    InitMounts().or_else([this](const Error& e) -> Result<> {
      log_.warn("Mount phase failed: {}", e.message);
      return {};
    });
  });

  Guarded(log_, "Network phase", [this] {
    InitNetwork().or_else([this](const Error& e) -> Result<> {
      log_.warn("Network phase failed: {}", e.message);
      return {};
    });
  });

  // Signals and the reaper come before any service starts, so exits during
  // StartAll are collected and a shutdown request works from here on.
  Guarded(log_, "Signal init", [this] { std::ignore = InitSignals(); });

  const bool stack_ok = Guarded(log_, "Service stack init", [this] {
    if (auto r = InitServiceStack(); !r) {
      throw std::runtime_error(r.error().message);
    }
  });
  if (!stack_ok) { service_manager_.reset(); }

  StartReaperThread();

  // From here on 'd' on the console opens a shell at any time; boot does not
  // wait for it. aember.shell=1 on the kernel command line opens one now.
  StartDebugShell(DebugShell::RequestedOnCmdline());

  if (service_manager_) {
    Guarded(log_, "Loading services", [this] {
      LoadAndRegisterProcesses("/etc/aember/services.json")
          .or_else([this](const Error& e) -> Result<> {
            log_.warn("{}", e.message);
            return {};
          });
    });

    Guarded(log_, "Loading containers", [this] {
      LoadAndRegisterContainers("/etc/aember/containers.json")
          .or_else([this](const Error& e) -> Result<> {
            log_.warn("{}", e.message);
            return {};
          });
    });

    Guarded(
        log_, "Starting services", [this] { service_manager_->StartAll(); });
  } else {
    log_.error("No service manager: services and containers are not started");
  }

  Guarded(log_, "Monitor and heartbeat", [this] {
    // AemberMonitor reads PID1 stats from /proc.
    monitor_ = std::make_unique<aember::device_health::AemberMonitor>();
    heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
    heartbeat_->Start();
  });

  log_.info("Aember Init System started successfully");

  RunLoop();
  Stop();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  // Gives the console back; a running shell is killed with everything else.
  if (debug_shell_) { debug_shell_->Stop(); }

  // Services stop while the reaper still runs: it reports their exits.
  Guarded(log_, "Stopping services", [this] {
    if (service_manager_) { service_manager_->StopAll(); }
  });

  // Join before the managers it calls into are destroyed below.
  StopReaperThread();

  signal_handler_.Stop();
  child_supervisor_.StopAll();
  cv_.notify_all();

  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }
  if (network_manager_) {
    network_manager_->Stop();
    network_manager_.reset();
  }

  monitor_.reset();

  // Destroy in reverse construction order.
  service_manager_.reset();
  dependency_resolver_.reset();
  process_manager_.reset();
  container_manager_.reset();
  mount_manager_.reset();

  log_.info("Aember Init System stopped");
}

// ---------------------------------------------------------------------------
// Init phases
// ---------------------------------------------------------------------------

AemberInit::Result<> AemberInit::InitMounts() {
  mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();

  if (!mount_manager_->MountEarlyFilesystems()) {
    return std::unexpected(Error{"Some early filesystems failed to mount"});
  }

  if (options_.chroot) {
    log_.info("chroot mode: not loading kernel modules (host kernel)");
    return {};
  }

  module_loader_.emplace();
  auto modules = module_loader_->LoadModules("/etc/aember/modules.json");
  if (!module_loader_->Load(modules)) {
    log_.warn("Some kernel modules failed to load, continuing anyway");
  }

  return {};
}

AemberInit::Result<> AemberInit::InitNetwork() {
  std::ifstream net_file("/etc/aember/network.json");
  if (!net_file.is_open()) {
    return std::unexpected(
        Error{"No network config found at /etc/aember/network.json"});
  }

  nlohmann::json net_cfg;
  try {
    net_file >> net_cfg;
  } catch (const std::exception& e) {
    return std::unexpected(
        Error{std::format("Failed to parse network config: {}", e.what())});
  }

  // Throws on an invalid config or a required interface that stays down.
  try {
    network_manager_ = std::make_unique<aember::network::NetworkManager>(
        net_cfg, [this](const aember::utils::network::ConnectivityStatus& s) {
          OnNetworkStatus(s);
        });
    network_manager_->Start();
  } catch (const std::exception& e) {
    network_manager_.reset();
    return std::unexpected(
        Error{std::format("Network bring-up failed: {}", e.what())});
  }

  if (!network_manager_->WaitForConnectivity(std::chrono::seconds(30))) {
    log_.warn("No internet connectivity after 30s, continuing anyway");
  }

  return {};
}

AemberInit::Result<> AemberInit::InitServiceStack() {
  container_manager_ =
      std::make_shared<aember::container_manager::ContainerManager>(
          mount_manager_,
          [this](const std::string& name,
                 ContainerState old_state,
                 ContainerState new_state) {
            OnContainerStateChange(name, old_state, new_state);
          });

  process_manager_ = std::make_unique<aember::process_manager::ProcessManager>(
      [this](const std::string& name, int pid, int exit_code) {
        log_.info(
            "Process '{}' (pid={}) exited (code={})", name, pid, exit_code);
        service_manager_->MirrorState(name, ProcessState::Failed);
      });

  dependency_resolver_ =
      std::make_unique<aember::service_manager::DependencyResolver>(
          [this](const std::string& name) {
            return service_manager_ &&
                   service_manager_->GetState(name) == ProcessState::Running;
          });

  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      *process_manager_,
      *container_manager_,
      *dependency_resolver_,
      child_supervisor_);

  // Wire restart callback — ProcessManager calls this after restart delay
  // which triggers ServiceManager to re-launch the process.
  process_manager_->SetRestartCallback([this](const std::string& name) {
    log_.info("Restarting process '{}'", name);
    service_manager_->Start(name);
  });

  service_manager_->SetStateChangeCallback([this](const std::string& name,
                                                  ProcessState old_state,
                                                  ProcessState new_state) {
    OnServiceStateChange(name, old_state, new_state);
  });

  return {};
}

AemberInit::Result<> AemberInit::InitSignals() {
  // Handlers run on the signal thread: they only record the request and
  // wake the main thread, which does the actual Stop(). Calling Stop() here
  // would join the signal thread from itself (std::system_error, terminate).
  // Same signals as BusyBox init, so its reboot/poweroff/halt work.
  signal_handler_.Register(
      SIGTERM, [this](int) { RequestShutdown(ShutdownAction::kReboot); });
  signal_handler_.Register(
      SIGINT, [this](int) { RequestShutdown(ShutdownAction::kReboot); });
  signal_handler_.Register(
      SIGUSR2, [this](int) { RequestShutdown(ShutdownAction::kPowerOff); });
  signal_handler_.Register(
      SIGUSR1, [this](int) { RequestShutdown(ShutdownAction::kHalt); });
  signal_handler_.Register(SIGHUP, [this](int) { /* reload config */ });

  signal_handler_.Register(SIGCHLD, [this](int) {
    {
      std::lock_guard lock{reap_mtx_};
      reap_pending_ = true;
    }
    reap_cv_.notify_one();
  });

  signal_handler_.Start();
  return {};
}

AemberInit::Result<> AemberInit::LoadAndRegisterProcesses(
    std::string_view path) {
  auto configs = process_manager_->LoadProcesses(std::string{path});
  log_.info("Loaded {} process config(s) from {}", configs.size(), path);

  const auto registered =
      configs | std::views::filter([&](const auto& cfg) {
        if (!process_manager_->AddProcess(cfg)) {
          log_.error("Failed to register process '{}'", cfg.name);
          return false;
        }
        return true;
      }) |
      std::views::transform([&](const auto& cfg) { return cfg.name; }) |
      std::ranges::to<std::vector>();

  std::ranges::for_each(registered, [&](const auto& name) {
    if (!service_manager_->AddProcess(name)) {
      log_.error("Failed to add process '{}' to service manager", name);
    } else {
      log_.info("Registered process '{}'", name);
    }
  });

  return {};
}

AemberInit::Result<> AemberInit::LoadAndRegisterContainers(
    std::string_view path) {
  auto configs = container_manager_->LoadContainers(std::string{path});
  log_.info("Loaded {} container config(s) from {}", configs.size(), path);

  std::vector<std::string> registered;
  for (const auto& cfg : configs) {
    // Only call AddContainer if LoadContainers didn't already register it
    if (!container_manager_->HasContainer(cfg.name)) {
      if (!container_manager_->AddContainer(cfg)) {
        log_.error("Failed to register container '{}'", cfg.name);
        continue;
      }
    }
    registered.push_back(cfg.name);
  }

  for (const auto& name : registered) {
    if (!service_manager_->AddContainer(name)) {
      log_.error("Failed to add container '{}' to service manager", name);
    } else {
      log_.info("Registered container '{}'", name);
    }
  }

  return {};
}

void AemberInit::StartReaperThread() {
  if (reaper_thread_.joinable()) return;

  reaper_thread_ = std::jthread{[this](std::stop_token st) {
    while (!st.stop_requested()) {
      {
        std::unique_lock lock{reap_mtx_};
        reap_cv_.wait_for(lock, st, std::chrono::seconds(1), [this] {
          return reap_pending_;
        });
        reap_pending_ = false;
      }
      Guarded(log_, "Reaping", [this] { ReapOnce(); });
    }
  }};
}

void AemberInit::StartDebugShell(bool shell_now) {
  Guarded(log_, "Debug shell", [this, shell_now] {
    debug_shell_.emplace();
    // The reaper collects the shell: track it, and it is alive until reaped.
    debug_shell_->Start(
        [this](pid_t pid) { child_supervisor_.AddChild(pid, "debug-shell"); },
        [this](pid_t pid) { return child_supervisor_.IsTracked(pid); },
        shell_now);
  });
}

void AemberInit::StopReaperThread() {
  if (!reaper_thread_.joinable()) return;
  reaper_thread_.request_stop();
  reaper_thread_.join();
}

void AemberInit::ReapOnce() {
  for (const auto& exit : child_supervisor_.ReapZombies()) {
    if (exit.tracked) {
      log_.debug(
          "Reaped '{}' (pid={} status={})", exit.name, exit.pid, exit.status);
    } else {
      log_.debug("Reaped orphan pid={} status={}", exit.pid, exit.status);
    }
    if (service_manager_) {
      service_manager_->HandleExit(exit.pid, exit.status);
    }
  }
}

void AemberInit::RequestShutdown(ShutdownAction action) {
  auto expected = ShutdownAction::kNone;
  if (!shutdown_.compare_exchange_strong(expected, action)) return;

  log_.info("Shutdown requested: {}", pid1::ToString(action));
  { std::lock_guard lock{mtx_}; }
  cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void AemberInit::RunLoop() {
  log_.info("Aember Init run loop started");
  std::unique_lock lock{mtx_};
  while (running_.load() && shutdown_.load() == ShutdownAction::kNone) {
    cv_.wait_for(lock, std::chrono::seconds(1));
  }
  log_.info("Aember Init run loop exiting");
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void AemberInit::OnHeartbeat(const nlohmann::json& base) {
  if (!monitor_) return;

  // Collect PID1 stats via /proc.
  auto payload = monitor_->Snapshot();

  // Enrich with network status.
  if (network_manager_) {
    const auto status = network_manager_->GetStatus();
    const auto net_info = network_manager_->GetNetworkInfo();
    payload["network"] = {
        {"online", status.online},
        {"interface", status.interface},
        {"rtt_ms", status.rtt_ms},
        {"ip", net_info.ip_address},
    };
  }

  // Enrich with service states.
  if (service_manager_) {
    auto services = nlohmann::json::array();
    for (const auto& name : service_manager_->GetNames()) {
      const auto state = service_manager_->GetState(name);
      const auto type =
          service_manager_->IsContainer(name) ? "container" : "process";
      services.push_back({
          {"name", name},
          {"state", aember::utils::process::ToString(state)},
          {"type", type},
      });
    }
    payload["services"] = services;
  }

  // Merge heartbeat base fields (timestamp, status).
  payload.merge_patch(base);

  // Log the enriched dashboard.
  monitor_->Log(payload);
  monitor_->LogToFile(payload, "/var/log/aember-monitor.json");
}

void AemberInit::OnServiceStateChange(const std::string& name,
                                      ProcessState old_state,
                                      ProcessState new_state) {
  log_.info("Service '{}': {} -> {}",
            name,
            aember::utils::process::ToString(old_state),
            aember::utils::process::ToString(new_state));
}

void AemberInit::OnNetworkStatus(
    const aember::utils::network::ConnectivityStatus& status) {
  if (!status.online) { log_.warn("Network: internet connectivity lost"); }
}

void AemberInit::OnContainerStateChange(const std::string& name,
                                        ContainerState old_state,
                                        ContainerState new_state) {
  log_.info("Container '{}': {} -> {}",
            name,
            aember::utils::container::ContainerStateToString(old_state),
            aember::utils::container::ContainerStateToString(new_state));
}

}  // namespace aember::aember_init
