#pragma once

#include <aember-libs/device-health/heartbeat.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace aember_test::device_health {

class HeartbeatTest : public ::testing::Test {
 protected:
  void SetUp() override {
    call_count_.store(0);
    last_heartbeat_data_.clear();
    all_heartbeat_data_.clear();
  }

  void TearDown() override {
    if (heartbeat_) {
      heartbeat_->Stop();
      heartbeat_.reset();
    }
  }

  // Helper: Simple callback that counts calls
  std::function<void(const nlohmann::json&)> GetCountingCallback() {
    return [this](const nlohmann::json& data) {
      call_count_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(data_mutex_);
      last_heartbeat_data_ = data;
    };
  }

  // Helper: Callback that stores all heartbeats
  std::function<void(const nlohmann::json&)> GetStoringCallback() {
    return [this](const nlohmann::json& data) {
      call_count_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(data_mutex_);
      all_heartbeat_data_.push_back(data);
    };
  }

  // Helper: Wait for a specific number of heartbeats
  bool WaitForHeartbeats(int expected_count,
                         std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (call_count_.load(std::memory_order_relaxed) < expected_count) {
      if (std::chrono::steady_clock::now() - start > timeout) { return false; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  std::unique_ptr<aember::device_health::Heartbeat> heartbeat_;
  std::atomic<int> call_count_{0};
  nlohmann::json last_heartbeat_data_;
  std::vector<nlohmann::json> all_heartbeat_data_;
  std::mutex data_mutex_;
};

}  // namespace aember_test::device_health
