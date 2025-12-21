/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @brief Library implementation for AemberInit
 * @version 0.1
 * @date 2025-07-18
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
  if (running_.load()) return;

  aember::utils::init_early_logging();

  log_.info("Starting Aember Init System");

  // Mount early filesystems (additional ones beyond /proc, /sys, /dev)
  mount_manager_.emplace();
  if (!mount_manager_->MountEarlyFilesystems()) {
    log_.warn("Some early filesystems failed to mount, continuing anyway");
  }

  // Initialize service manager
  service_manager_ = std::make_unique<aember::service_manager::ServiceManager>(
      child_supervisor_);

  // Set up service state change callback
  service_manager_->SetStateChangeCallback(
      std::bind(&AemberInit::OnServiceStateChangeCallback,
                this,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3));

  // Setup signal handlers
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
    // TODO: reload config later
  });

  signal_handler_.Register(SIGCHLD, [this](int) {
    log_.debug("SIGCHLD received - reaping children");

    // Reap all children
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

      // Notify service manager about the exit
      service_manager_->HandleServiceExit(pid, exit_code);
    }

    // Also let child supervisor handle it (for logging)
    child_supervisor_.HandleSIGCHLD();
  });

  signal_handler_.Start();

  running_.store(true);

  // Start heartbeat for health monitoring
  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
  heartbeat_->Start();

  // TODO: Load service configurations from file
  // For now, you can add services programmatically for testing:
  /*
  aember::service_manager::ServiceConfig test_service("test-service",
  "/bin/sleep"); test_service.args = {"10"}; test_service.restart_policy =
  aember::service_manager::RestartPolicy::ALWAYS;
  service_manager_->AddService(test_service);
  service_manager_->StartService("test-service");
  */

  log_.info("Aember Init System started successfully");

  RunLoop();

  Stop();
}

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  // Stop all services first
  if (service_manager_) { service_manager_->StopAll(); }

  // Stop signal handling
  signal_handler_.Stop();

  // Stop all remaining child processes
  child_supervisor_.StopAll();

  // Wake up RunLoop if blocked
  cv_.notify_all();

  // Stop and clean up heartbeat
  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }

  // Clean up service manager
  if (service_manager_) { service_manager_.reset(); }

  // Unmount filesystems (if needed during shutdown)
  // Note: Usually you don't unmount during normal operation
  // This would be used during system shutdown/reboot
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

  while (running_.load()) {
    cv_.wait_for(lock, std::chrono::seconds(1));

    // TODO: Here you can add periodic tasks like:
    // - Check service health
    // - Process pending events
    // - Handle configuration changes
    // - Monitor system resources
  }

  log_.info("Aember Init run loop exiting");
}

}  // namespace aember::aember_init
