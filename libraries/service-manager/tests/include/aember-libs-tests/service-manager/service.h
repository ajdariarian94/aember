#pragma once

#include <aember-libs/service-manager/service.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace aember_test::service_manager {

class ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.name = "test-service";
    config_.command = "/bin/echo";
    config_.args = {"hello"};
    config_.working_directory = "/tmp";
    config_.environment = {{"TEST_VAR", "test_value"}};
    config_.restart_policy = aember::service_manager::RestartPolicy::NEVER;
    config_.max_restart_attempts = 3;
    config_.restart_delay = std::chrono::seconds(1);
  }

  void TearDown() override {
    // Cleanup if needed
  }

  aember::service_manager::ServiceConfig config_;
};

}  // namespace aember_test::service_manager
