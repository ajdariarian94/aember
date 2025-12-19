/**
 * @file aember-init.cpp
 * @author Arian Ajdari
 * @brief Library implementation for AemberInit
 * @version 0.1
 * @date 2025-07-18
 */

#include <aember-libs/aember-init/aember-init.h>

#include <spdlog/spdlog.h>
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
    child_supervisor_.HandleSIGCHLD();
  });

  signal_handler_.Start();

  running_.store(true);

  // Start heartbeat for health monitoring
  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1),
      std::chrono::seconds(1));
  heartbeat_->Start();

  log_.info("Aember Init System started successfully");

  RunLoop();

  Stop();
}

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  log_.info("Stopping Aember Init System...");

  // Stop signal handling first
  signal_handler_.Stop();

  // Stop all child processes
  child_supervisor_.StopAll();

  // Wake up RunLoop if blocked
  cv_.notify_all();

  // Stop and clean up heartbeat
  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }

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

void AemberInit::RunLoop() {
  log_.info("Aember Init run loop started");

  std::unique_lock<std::mutex> lock(mtx_);

  while (running_.load()) {
    cv_.wait_for(lock, std::chrono::seconds(1));

    // TODO: Here you can add periodic tasks like:
    // - Check container health
    // - Process pending events
    // - Handle configuration changes
  }

  log_.info("Aember Init run loop exiting");
}

}  // namespace aember::aember_init
