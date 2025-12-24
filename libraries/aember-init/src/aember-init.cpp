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

AemberInit::AemberInit() : running_(false), log_("aember-init") {}

AemberInit::~AemberInit() {
  Stop();
}

void AemberInit::Start() {
  // Prevent multiple starts
  if (running_.load()) return;

  // Initialize logging as early as possible
  aember::utils::init_early_logging();
  log_.info("Starting Aember Init System");

  /**
   * Mount early filesystems.
   * These are required before services start, after kernel mounted /proc, /sys,
   * /dev.
   */
  mount_manager_.emplace();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  /**
   * Pivot to real root filesystem if we're in initramfs.
   */
  if (aember::root_manager::RootManager::IsInInitramfs()) {
    log_.info("Initramfs detected, preparing to pivot to real root");

    root_manager_ =
        std::make_unique<aember::root_manager::RootManager>(*mount_manager_);

    // Configure the real root filesystem
    aember::root_manager::RootConfig root_config;
    root_config.device =
        "/dev/sda1";  // TODO: parse /proc/cmdline or use UUID/LABEL
    root_config.fstype = "ext4";
    root_config.mount_options = "rw";
    root_config.new_root_path = "/mnt/root";

    // Ensure pivot mount point exists
    if (!mount_manager_->EnsureDirectory(root_config.new_root_path)) {
      log_.error("Failed to create pivot mount point: {}",
                 root_config.new_root_path);
    }

    // Perform pivot
    if (!root_manager_->PerformPivot(root_config)) {
      log_.warn("Pivot to real root failed; continuing in initramfs");
    } else {
      log_.info("Successfully pivoted to real root filesystem");
    }
  } else {
    log_.info("Not in initramfs, skipping pivot_root");
  }

  /**
   * Initialize the service manager.
   */
  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      child_supervisor_);

  /**
   * Register callback for service state changes.
   */
  service_manager_->SetStateChangeCallback(
      std::bind(&AemberInit::OnServiceStateChangeCallback,
                this,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3));

  /**
   * Load service configuration from the real root.
   */
  config_manager_ = std::make_unique<aember::config_manager::ConfigManager>();
  std::string config_path = "/etc/aember/services.json";

  if (config_manager_->LoadFromFile(config_path)) {
    log_.info("Loaded service configuration from {}", config_path);

    // Add all configured services
    for (const auto& service_config : config_manager_->GetServices()) {
      if (service_manager_->AddService(service_config)) {
        log_.info("Added service: {}", service_config.name);
      } else {
        log_.error("Failed to add service: {}", service_config.name);
      }
    }

    // Start all services
    service_manager_->StartAll();
  } else {
    log_.warn("No service configuration found at {}", config_path);
  }

  /**
   * Register signal handlers.
   */
  signal_handler_.Register(SIGTERM, [this](int) {
    log_.info("SIGTERM received - shutting down");
    Stop();
  });

  signal_handler_.Register(SIGINT, [this](int) {
    log_.info("SIGINT received - shutting down");
    Stop();
  });

  signal_handler_.Register(SIGHUP, [this](int) {
    log_.info("SIGHUP received - reload configuration");
    // TODO: Reload configuration
  });

  signal_handler_.Register(SIGCHLD, [this](int) {
    log_.debug("SIGCHLD received - reaping children");

    while (true) {
      int status = 0;
      pid_t pid = ::waitpid(-1, &status, WNOHANG);
      if (pid <= 0) break;

      int exit_code = 0;
      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
      }

      service_manager_->HandleServiceExit(pid, exit_code);
    }

    child_supervisor_.HandleSIGCHLD();
  });

  // Start signal handling loop
  signal_handler_.Start();
  running_.store(true);

  /**
   * Start heartbeat monitoring.
   */
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
