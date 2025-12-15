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

  signal_handler_.Register(SIGTERM, [this](int) {
    log_.info("SIGTERM received");
    Stop();
  });

  signal_handler_.Register(SIGINT, [this](int) {
    log_.info("SIGINT received");
    Stop();
  });

  signal_handler_.Register(SIGHUP, [this](int) {
    log_.info("SIGHUP received (reload)");
    // reload config later
  });

  signal_handler_.Register(SIGCHLD, [this](int) {
    log_.debug("SIGCHLD received");
    child_supervisor_.HandleSIGCHLD();
  });

  signal_handler_.Start();

  log_.info("Initializing Aember");

  running_.store(true);

  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
  heartbeat_->Start();

  RunLoop();

  Stop();
}

void AemberInit::Stop() {
  if (!running_.exchange(false)) return;

  // Stop signal handling first
  signal_handler_.Stop();

  child_supervisor_.StopAll();

  // Wake up RunLoop if blocked
  cv_.notify_all();

  // Stop and clean up heartbeat
  if (heartbeat_) {
    heartbeat_->Stop();
    heartbeat_.reset();
  }

  log_.info("Aember stopped");
}

void AemberInit::HeartbeatCallback(const nlohmann::json& heartbeat_payload) {
  log_.info("{}", heartbeat_payload.dump());
}

void AemberInit::RunLoop() {
  log_.info("AemberInit run loop started");

  std::unique_lock<std::mutex> lock(mtx_);

  while (running_.load()) { cv_.wait_for(lock, std::chrono::seconds(1)); }

  log_.info("AemberInit run loop exiting");
}

}  // namespace aember::aember_init
