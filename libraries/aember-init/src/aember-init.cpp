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

  log_.info("Initializing Aember");

  running_.store(true);

  // Initialize heartbeat (no beeping)
  heartbeat_.emplace(
      std::bind(&AemberInit::HeartbeatCallback, this, std::placeholders::_1));
  heartbeat_->Start();

  // Run loop in the same thread (blocks here)
  RunLoop();

  // After RunLoop exits, clean up
  Stop();
}

void AemberInit::Stop() {
  if (!running_.load()) return;

  running_.store(false);

  // Wake the RunLoop thread so it stops
  cv_.notify_all();

  if (worker_thread_.joinable()) { worker_thread_.join(); }

  heartbeat_.reset();
}

void AemberInit::HeartbeatCallback(const nlohmann::json& heartbeat_payload) {
  log_.info(heartbeat_payload.dump());
}

void AemberInit::RunLoop() {
  log_.info("AemberInit run loop started");

  std::unique_lock<std::mutex> lock(mtx_);

  while (running_.load()) { cv_.wait_for(lock, std::chrono::seconds(1)); }

  log_.info("AemberInit run loop exiting");
}

}  // namespace aember::aember_init
