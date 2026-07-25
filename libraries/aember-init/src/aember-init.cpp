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

#include <sys/wait.h>

namespace aember::aember_init {

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

AemberInit::AemberInit(std::string_view logger_name)
    : log_(std::string{logger_name}) {
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
  if (running_.load()) return;

  log_.info("Starting Aember in initramfs mode");

  mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();
  mount_manager_->MountInitramfsFilesystems();

  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  log_.info("Initramfs detected, preparing to pivot to real root");

  root_manager_ =
      std::make_unique<aember::root_manager::RootManager>(mount_manager_);

  aember::utils::root::RootConfig root_config;
  root_config.ParseFromProcCmdline();

  if (!mount_manager_->EnsureDirectory(root_config.new_root_path)) {
    log_.error("Failed to create pivot mount point: {}",
               root_config.new_root_path);
  }

  if (!root_manager_->PerformPivot(root_config)) {
    log_.warn("Pivot to real root failed; continuing in initramfs");

    heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
    heartbeat_->Start();

    log_.info("Aember Init System started successfully");
    RunLoop();
    Stop();
  }
}

// ---------------------------------------------------------------------------
// StartRoot
// ---------------------------------------------------------------------------

void AemberInit::StartRoot() {
  if (running_.load()) return;

  aember::utils::logging::enable_file_logging("/var/log/aember-init.log");
  log_.info("Starting Aember in root mode");

  debug_shell_.emplace();
  const bool spawn_debug_shell =
      debug_shell_ && debug_shell_->CheckDebugShell();

  InitMounts().or_else([this](const Error& e) -> Result<> {
    log_.warn("Mount phase failed: {}", e.message);
    return {};
  });

  InitNetwork().or_else([this](const Error& e) -> Result<> {
    log_.warn("Network phase failed: {}", e.message);
    return {};
  });

  if (auto r = InitServiceStack(); !r) {
    log_.error("Service stack init failed: {}", r.error().message);
    return;
  }

  LoadAndRegisterProcesses("/etc/aember/services.json")
      .or_else([this](const Error& e) -> Result<> {
        log_.warn("{}", e.message);
        return {};
      });

  LoadAndRegisterContainers("/etc/aember/containers.json")
      .or_else([this](const Error& e) -> Result<> {
        log_.warn("{}", e.message);
        return {};
      });

  service_manager_->StartAll();

  InitSignals().or_else([this](const Error& e) -> Result<> {
    log_.error("Signal init failed: {}", e.message);
    return {};
  });

  running_.store(true);

  // Initialize AemberMonitor — reads PID1 stats from /proc.
  monitor_ = std::make_unique<aember::device_health::AemberMonitor>();

  heartbeat_.emplace([this](const nlohmann::json& p) { OnHeartbeat(p); });
  heartbeat_->Start();

  log_.info("Aember Init System started successfully");

  if (spawn_debug_shell) {
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });
    aember::utils::logging::enable_console_silence();
    debug_shell_->SilenceAemberInBackground();
    debug_shell_->SpawnDebugShell();
  }

  RunLoop();
  Stop();
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  if (service_manager_) { service_manager_->StopAll(); }

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

  network_manager_ = std::make_unique<aember::network::NetworkManager>(
      net_cfg, [this](const aember::utils::network::ConnectivityStatus& s) {
        OnNetworkStatus(s);
      });

  network_manager_->Start();

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
  signal_handler_.Register(SIGTERM, [this](int) { Stop(); });
  signal_handler_.Register(SIGINT, [this](int) { Stop(); });
  signal_handler_.Register(SIGHUP, [this](int) { /* reload config */ });

  signal_handler_.Register(SIGCHLD, [this](int) {
    log_.info("SIGCHLD received");  // ← add this
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      log_.info("Reaped pid={} status={}", pid, status);
      service_manager_->HandleExit(pid, status);
      child_supervisor_.RemoveChild(pid);
    }
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

  const auto registered =
      configs | std::views::filter([&](const auto& cfg) {
        if (!container_manager_->AddContainer(cfg)) {
          log_.error("Failed to register container '{}'", cfg.name);
          return false;
        }
        return true;
      }) |
      std::views::transform([&](const auto& cfg) { return cfg.name; }) |
      std::ranges::to<std::vector>();

  std::ranges::for_each(registered, [&](const auto& name) {
    if (!service_manager_->AddContainer(name)) {
      log_.error("Failed to add container '{}' to service manager", name);
    } else {
      log_.info("Registered container '{}'", name);
    }
  });

  return {};
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void AemberInit::RunLoop() {
  log_.info("Aember Init run loop started — periodic reaping active");
  std::unique_lock lock{mtx_};
  while (running_.load()) {
    cv_.wait_for(lock, std::chrono::seconds(1));
    log_.debug("RunLoop tick — checking for zombies");  // ← add this
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      log_.info("RunLoop reaped zombie pid={}", pid);
      service_manager_->HandleExit(pid, status);
      child_supervisor_.RemoveChild(pid);
    }
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
