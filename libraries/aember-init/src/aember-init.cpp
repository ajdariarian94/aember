/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @date 2025-12-22
 * @brief Implementation of the AemberInit init system.
 *
 * This file contains the implementation of the top-level init process.
 * It is responsible for early system setup, service supervision,
 * signal handling, and maintaining the system run loop.
 */

#include <aember-libs/aember-init/aember-init.h>

#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <nlohmann/json.hpp>

#include <fstream>

namespace aember::aember_init {

AemberInit::AemberInit(const std::string& logger_name)
    : running_(false), log_(logger_name) {
#ifndef NDEBUG
  spdlog::set_level(spdlog::level::debug);
#else
  spdlog::set_level(spdlog::level::info);
#endif
}

AemberInit::~AemberInit() {
  Stop();
}

void AemberInit::StartInitramfs() {
  if (running_.load()) return;

  log_.info("Starting Aember in initramfs mode");

  // ----------------------------
  // Mount early filesystems
  // ----------------------------
  mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  log_.info("Initramfs detected, preparing to pivot to real root");

  root_manager_ =
      std::make_unique<aember::root_manager::RootManager>(*mount_manager_);

  aember::root_manager::RootConfig root_config;
  root_config.ParseFromProcCmdline();

  if (!mount_manager_->EnsureDirectory(root_config.new_root_path)) {
    log_.error("Failed to create pivot mount point: {}",
               root_config.new_root_path);
  }

  if (!root_manager_->PerformPivot(root_config)) {
    log_.warn("Pivot to real root failed; continuing in initramfs");

    // ----------------------------
    // Start heartbeat
    // ----------------------------
    heartbeat_.emplace(
        std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
    heartbeat_->Start();

    log_.info("Aember Init System started successfully");

    // Main run loop
    RunLoop();

    Stop();
  }
}

void AemberInit::StartRoot() {
  if (running_.load()) return;

  log_.info("Starting Aember in root mode");

  // ----------------------------
  // Mount early filesystems
  // ----------------------------
  mount_manager_ = std::make_shared<aember::mount_manager::MountManager>();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  // ----------------------------
  // Load kernel modules
  // ----------------------------
  module_loader_.emplace();
  if (!module_loader_->LoadFromConfig("/etc/aember/modules.json")) {
    log_.warn("Some kernel modules failed to load, continuing anyway");
  }

  // ----------------------------
  // Enable file logging
  // ----------------------------
  aember::utils::logging::enable_file_logging("/var/log/aember-init.log");

  debug_shell_.emplace();
  if (!debug_shell_) { log_.warn("Debug Shell not available"); }

  bool debug_shell = debug_shell_->CheckDebugShell();

  // ----------------------------
  // Network
  // ----------------------------
  nlohmann::json net_cfg;
  std::string net_config_path = "/etc/aember/network.json";

  std::ifstream net_file(net_config_path);
  if (net_file.is_open()) {
    try {
      net_file >> net_cfg;

      network_manager_ = std::make_unique<aember::network::NetworkManager>(
          net_cfg,
          std::bind(&AemberInit::OnNetworkStatusCallback,
                    this,
                    std::placeholders::_1));

      network_manager_->Start();

      if (!network_manager_->WaitForConnectivity(std::chrono::seconds(30))) {
        log_.warn("No internet connectivity after 60s, continuing anyway");
      }

    } catch (const std::exception& e) {
      log_.error("Failed to initialize NetworkManager: {}", e.what());
    }
  } else {
    log_.warn("No network config found at {}, skipping network setup",
              net_config_path);
  }

  // ----------------------------
  // Initialize Container Manager 🔥
  // ----------------------------
  container_manager_ =
      std::make_shared<aember::container_manager::ContainerManager>(
          mount_manager_,
          std::bind(&AemberInit::OnContainerStateCallback,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2,
                    std::placeholders::_3));

  // ----------------------------
  // Initialize Service Manager
  // ----------------------------
  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      child_supervisor_, container_manager_);

  service_manager_->SetStateChangeCallback(
      std::bind(&AemberInit::OnServiceStateChangeCallback,
                this,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3));

  // ----------------------------
  // Load service configuration
  // ----------------------------
  config_manager_ = std::make_unique<aember::config_manager::ConfigManager>();
  std::string config_path = "/etc/aember/services.json";

  if (config_manager_->LoadFromFile(config_path)) {
    log_.info("Loaded service configuration from {}", config_path);

    for (const auto& service_config : config_manager_->GetServices()) {
      if (service_manager_->AddService(service_config)) {
        log_.info("Added service: {}", service_config.name);
      } else {
        log_.error("Failed to add service: {}", service_config.name);
      }
    }

    if (debug_shell) {
      spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });
      aember::utils::logging::enable_console_silence();
      debug_shell_->SilenceAemberInBackground();

      service_manager_->StartAll();
      debug_shell_->SpawnDebugShell();
    } else {
      service_manager_->StartAll();
    }
  } else {
    log_.warn("No service configuration found at {}", config_path);
  }

  // ----------------------------
  // Signals
  // ----------------------------
  signal_handler_.Register(SIGTERM, [this](int) { Stop(); });
  signal_handler_.Register(SIGINT, [this](int) { Stop(); });
  signal_handler_.Register(SIGHUP, [this](int) { /* reload config */ });

  signal_handler_.Register(SIGCHLD, [this](int) {
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      service_manager_->HandleServiceExit(pid, WEXITSTATUS(status));
    }

    child_supervisor_.HandleSIGCHLD();
  });

  signal_handler_.Start();
  running_.store(true);

  // ----------------------------
  // Heartbeat
  // ----------------------------
  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
  heartbeat_->Start();

  log_.info("Aember Init System started successfully");

  RunLoop();
  Stop();
}

void AemberInit::Stop() {
  // Ensure Stop is only executed once
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  /**
   * Stop all managed services first.
   *
   * This ensures services are terminated cleanly before tearing down
   * infrastructure.
   */
  if (service_manager_) { service_manager_->StopAll(); }

  // Stop signal handling
  signal_handler_.Stop();

  // Terminate any remaining child processes
  child_supervisor_.StopAll();

  // Wake the run loop if it is blocked
  cv_.notify_all();

  /**
   * Stop and destroy the heartbeat subsystem.
   */
  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }

  // Stop network manager
  if (network_manager_) {
    network_manager_->Stop();
    network_manager_.reset();
  }

  // Destroy the service manager
  if (service_manager_) { service_manager_.reset(); }

  /**
   * Unmount filesystems if required.
   *
   * Typically used during system shutdown or reboot.
   */
  if (mount_manager_) {
    // mount_manager_->UnmountAll(false);
    mount_manager_.reset();
  }

  log_.info("Aember Init System stopped");
}

void AemberInit::HeartbeatCallback(const nlohmann::json& heartbeat_payload) {
  log_.info("Heartbeat: {}", heartbeat_payload.dump());
}

void AemberInit::OnServiceStateChangeCallback(
    const std::string& name, aember::utils::service::ServiceState old_state,
    aember::utils::service::ServiceState new_state) {
  log_.info("Service '{}' state changed: {} -> {}",
            name,
            aember::utils::service::ServiceStateToString(old_state),
            aember::utils::service::ServiceStateToString(new_state));
}

void AemberInit::OnNetworkStatusCallback(
    const aember::network::ConnectivityStatus& status) {
  if (status.online) {
    log_.info(
        "Network: online via {} rtt={}ms", status.interface, status.rtt_ms);
  } else {
    log_.warn("Network: internet connectivity lost");
  }
}

void AemberInit::OnContainerStateCallback(
    const std::string& name,
    aember::utils::container::ContainerState old_state,
    aember::utils::container::ContainerState new_state) {
  log_.info("Container '{}' state changed: {} -> {}",
            name,
            aember::utils::container::ContainerStateToString(old_state),
            aember::utils::container::ContainerStateToString(new_state));

  // 🔥 Optional (next step): sync to ServiceManager
  // Example:
  //
  // if (service_manager_) {
  //   // map container state → service state
  // }
}

void AemberInit::RunLoop() {
  log_.info("Aember Init run loop started");

  std::unique_lock<std::mutex> lock(mtx_);

  /**
   * Main init loop.
   *
   * Keeps the init process alive and provides a place for periodic
   * maintenance tasks.
   */
  while (running_.load()) {
    cv_.wait_for(lock, std::chrono::seconds(1));

    // Future periodic tasks may include:
    // - Service health checks
    // - Processing pending events
    // - Configuration reloads
    // - System resource monitoring
  }

  log_.info("Aember Init run loop exiting");
}

}  // namespace aember::aember_init
