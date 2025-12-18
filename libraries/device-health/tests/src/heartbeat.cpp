#include <aember-libs-tests/device-health/heartbeat.h>

namespace aember_test::device_health {

TEST_F(HeartbeatTest, ConstructorAndDestructor) {
  auto callback = [](const nlohmann::json&) {};
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(100));
  // Should not crash
}

TEST_F(HeartbeatTest, StartStopBasic) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(100));

  heartbeat_->Start();
  heartbeat_->Stop();
  // Should not crash
}

TEST_F(HeartbeatTest, CallbackInvokedOnStart) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(100));

  heartbeat_->Start();

  // Wait for at least one heartbeat
  ASSERT_TRUE(WaitForHeartbeats(1, std::chrono::seconds(2)));
  EXPECT_GE(call_count_.load(std::memory_order_relaxed), 1);
}

TEST_F(HeartbeatTest, MultipleHeartbeatsWithInterval) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();

  // Wait for at least 3 heartbeats
  ASSERT_TRUE(WaitForHeartbeats(3, std::chrono::seconds(2)));
  EXPECT_GE(call_count_.load(std::memory_order_relaxed), 3);
}

TEST_F(HeartbeatTest, HeartbeatDataContainsStatus) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(1, std::chrono::seconds(1)));

  std::lock_guard<std::mutex> lock(data_mutex_);
  ASSERT_TRUE(last_heartbeat_data_.contains("status"));
  EXPECT_EQ(last_heartbeat_data_["status"], "online");
}

TEST_F(HeartbeatTest, HeartbeatDataContainsTimestamp) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(1, std::chrono::seconds(1)));

  std::lock_guard<std::mutex> lock(data_mutex_);
  ASSERT_TRUE(last_heartbeat_data_.contains("timestamp"));
  EXPECT_GT(last_heartbeat_data_["timestamp"].get<int64_t>(), 0);
}

TEST_F(HeartbeatTest, TimestampsAreIncreasing) {
  auto callback = GetStoringCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(3, std::chrono::seconds(2)));

  std::lock_guard<std::mutex> lock(data_mutex_);
  ASSERT_GE(all_heartbeat_data_.size(), 3);

  int64_t prev_timestamp = 0;
  for (const auto& data : all_heartbeat_data_) {
    ASSERT_TRUE(data.contains("timestamp"));
    int64_t timestamp = data["timestamp"].get<int64_t>();
    EXPECT_GT(timestamp, prev_timestamp);
    prev_timestamp = timestamp;
  }
}

TEST_F(HeartbeatTest, StopPreventsMoreHeartbeats) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(2, std::chrono::seconds(1)));

  int count_before_stop = call_count_.load(std::memory_order_relaxed);
  heartbeat_->Stop();

  // Wait a bit and ensure no more callbacks
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int count_after_stop = call_count_.load(std::memory_order_relaxed);

  EXPECT_EQ(count_before_stop, count_after_stop);
}

TEST_F(HeartbeatTest, MultipleStartCallsIgnored) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(100));

  heartbeat_->Start();
  heartbeat_->Start();  // Second call should be ignored
  heartbeat_->Start();  // Third call should be ignored

  ASSERT_TRUE(WaitForHeartbeats(1, std::chrono::seconds(1)));
  // Should not crash or create multiple threads
}

TEST_F(HeartbeatTest, MultipleStopCallsSafe) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(1, std::chrono::seconds(1)));

  heartbeat_->Stop();
  heartbeat_->Stop();  // Second call should be safe
  heartbeat_->Stop();  // Third call should be safe
  // Should not crash
}

TEST_F(HeartbeatTest, StopWithoutStartSafe) {
  auto callback = [](const nlohmann::json&) {};
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(100));

  heartbeat_->Stop();  // Should not crash
}

TEST_F(HeartbeatTest, RestartAfterStop) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  // First run
  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(2, std::chrono::seconds(1)));
  heartbeat_->Stop();

  int first_run_count = call_count_.load(std::memory_order_relaxed);

  // Reset counter
  call_count_.store(0);

  // Second run
  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(2, std::chrono::seconds(1)));

  EXPECT_GE(call_count_.load(std::memory_order_relaxed), 2);
}

TEST_F(HeartbeatTest, CallbackExceptionDoesNotCrash) {
  auto throwing_callback = [this](const nlohmann::json&) {
    call_count_.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("Test exception");
  };

  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      throwing_callback, std::chrono::milliseconds(50));

  heartbeat_->Start();

  // Should continue working despite exceptions
  ASSERT_TRUE(WaitForHeartbeats(3, std::chrono::seconds(2)));
  EXPECT_GE(call_count_.load(std::memory_order_relaxed), 3);
}

TEST_F(HeartbeatTest, IntervalTiming) {
  auto callback = GetStoringCallback();
  auto interval = std::chrono::milliseconds(100);
  heartbeat_ =
      std::make_unique<aember::device_health::Heartbeat>(callback, interval);

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(3, std::chrono::seconds(2)));

  std::lock_guard<std::mutex> lock(data_mutex_);
  ASSERT_GE(all_heartbeat_data_.size(), 3);

  // Check that intervals are roughly correct (with some tolerance)
  for (size_t i = 1; i < all_heartbeat_data_.size(); ++i) {
    int64_t t1 = all_heartbeat_data_[i - 1]["timestamp"].get<int64_t>();
    int64_t t2 = all_heartbeat_data_[i]["timestamp"].get<int64_t>();
    int64_t diff = t2 - t1;

    // Allow 50% tolerance for timing variations
    EXPECT_GE(diff, interval.count() / 2);
    EXPECT_LE(diff, interval.count() * 2);
  }
}

TEST_F(HeartbeatTest, DestructorStopsHeartbeat) {
  auto callback = GetCountingCallback();
  heartbeat_ = std::make_unique<aember::device_health::Heartbeat>(
      callback, std::chrono::milliseconds(50));

  heartbeat_->Start();
  ASSERT_TRUE(WaitForHeartbeats(2, std::chrono::seconds(1)));

  int count_before_destroy = call_count_.load(std::memory_order_relaxed);
  heartbeat_.reset();  // Destroy the heartbeat

  // Wait and ensure no more callbacks
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int count_after_destroy = call_count_.load(std::memory_order_relaxed);

  EXPECT_EQ(count_before_destroy, count_after_destroy);
}

}  // namespace aember_test::device_health
