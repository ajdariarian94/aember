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
   *
   * These are filesystems required before services start, but after
   * the kernel has mounted /proc, /sys, and /dev.
   */
  mount_manager_.emplace();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  /**
   * Initialize the service manager.
   *
   * The service manager owns all service definitions and coordinates
   * their lifecycle. It depends on the child supervisor for process
   * tracking.
   */
  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      child_supervisor_);

  /**
   * Register callback for observing service state changes.
   *
   * This is primarily used for logging and higher-level orchestration.
   */
  service_manager_->SetStateChangeCallback(
      std::bind(&AemberInit::OnServiceStateChangeCallback,
                this,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3));

  /**
   * Register signal handlers.
   *
   * Signals are handled centrally and dispatched to the appropriate
   * subsystems.
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
    // Configuration reload will be implemented later
  });

  /**
   * Handle child process termination.
   *
   * SIGCHLD is used to reap exited children and notify the service
   * manager about service exits.
   */
  signal_handler_.Register(SIGCHLD, [this](int) {
    log_.debug("SIGCHLD received - reaping children");

    // Reap all exited children
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

      // Notify the service manager about the service exit
      service_manager_->HandleServiceExit(pid, exit_code);
    }

    // Allow the child supervisor to perform its own bookkeeping/logging
    child_supervisor_.HandleSIGCHLD();
  });

  // Start signal dispatching
  signal_handler_.Start();

  // Mark init system as running
  running_.store(true);

  /**
   * Start the heartbeat subsystem.
   *
   * The heartbeat periodically emits system health information
   * while the init system is running.
   */
  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
  heartbeat_->Start();

  /**
   * Service loading will be driven by configuration in the future.
   *
   * For now, services can be added programmatically for testing.
   */
  /*
  aember::service_manager::ServiceConfig test_service(
      "test-service", "/bin/sleep");
  test_service.args = {"10"};
  test_service.restart_policy =
      aember::service_manager::RestartPolicy::ALWAYS;
  service_manager_->AddService(test_service);
  service_manager_->StartService("test-service");
  */

  log_.info("Aember Init System started successfully");

  // Enter the main run loop
  RunLoop();

  // Ensure clean shutdown when run loop exits
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
