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
  mount_manager_.emplace();
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
  mount_manager_.emplace();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  // ----------------------------
  // Enable file logging.
  // Injects the file sink into all existing loggers (including log_)
  // and stores it globally for any loggers created afterwards.
  // ----------------------------
  aember::utils::enable_file_logging("/var/log/aember-init.log");

  debug_shell_.emplace();
  if (!debug_shell_) { log_.warn("Debug Shell not available"); }

  // ----------------------------
  // Check debug shell
  // ----------------------------
  bool debug_shell = debug_shell_->CheckDebugShell();

  // ----------------------------
  // Initialize Service Manager
  // ----------------------------
  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      child_supervisor_);

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
      aember::utils::enable_console_silence();  // drop stdout sink cleanly
      debug_shell_
          ->SilenceAemberInBackground();  // now dup2 is just a safety net
      service_manager_->StartAll();
      debug_shell_->SpawnDebugShell();
    } else {
      service_manager_->StartAll();
    }
  } else {
    log_.warn("No service configuration found at {}", config_path);
  }

  // ----------------------------
  // Setup signal handlers
  // ----------------------------
  signal_handler_.Register(SIGTERM, [this](int) { Stop(); });
  signal_handler_.Register(SIGINT, [this](int) { Stop(); });
  signal_handler_.Register(SIGHUP, [this](int) { /* reload config */ });
  signal_handler_.Register(SIGCHLD, [this](int) {
    int status = 0;
    while (waitpid(-1, &status, WNOHANG) > 0) {
      service_manager_->HandleServiceExit(status, WEXITSTATUS(status));
    }
    child_supervisor_.HandleSIGCHLD();
  });

  signal_handler_.Start();
  running_.store(true);

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
    const std::string& name, aember::service_manager::ServiceState old_state,
    aember::service_manager::ServiceState new_state) {
  log_.info("Service '{}' state changed: {} -> {}",
            name,
            aember::service_manager::ServiceStateToString(old_state),
            aember::service_manager::ServiceStateToString(new_state));
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
