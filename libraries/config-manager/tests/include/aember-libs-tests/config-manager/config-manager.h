#pragma once

#include <aember-libs/config-manager/config-manager.h>

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

namespace aember_test::config_manager {

class ConfigManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    manager_ = std::make_unique<aember::config_manager::ConfigManager>();

    // Create a temporary directory for test files
    test_dir_ = "/tmp/config_test_" + std::to_string(getpid());
    mkdir(test_dir_.c_str(), 0755);
  }

  void TearDown() override {
    manager_.reset();

    // Clean up test files
    system(("rm -rf " + test_dir_).c_str());
  }

  // Helper: Create a test config file
  std::string CreateTestConfigFile(const std::string& filename,
                                   const std::string& content) {
    std::string path = test_dir_ + "/" + filename;
    std::ofstream file(path);
    file << content;
    file.close();
    return path;
  }

  // Helper: Basic valid config JSON
  std::string GetBasicValidConfig() {
    return R"({
      "services": [
        {
          "name": "test-service",
          "command": "/bin/echo"
        }
      ]
    })";
  }

  // Helper: Full config with all fields
  std::string GetFullConfig() {
    return R"({
      "services": [
        {
          "name": "full-service",
          "command": "/usr/bin/myapp",
          "args": ["--port", "8080", "--verbose"],
          "working_directory": "/var/app",
          "environment": {
            "LOG_LEVEL": "debug",
            "API_KEY": "secret123"
          },
          "restart_policy": "always",
          "max_restart_attempts": 10,
          "restart_delay_seconds": 5,
          "dependencies": ["network", "database"]
        }
      ]
    })";
  }

  // Helper: Multiple services config
  std::string GetMultipleServicesConfig() {
    return R"({
      "services": [
        {
          "name": "service1",
          "command": "/bin/service1"
        },
        {
          "name": "service2",
          "command": "/bin/service2",
          "dependencies": ["service1"]
        },
        {
          "name": "service3",
          "command": "/bin/service3",
          "restart_policy": "on-failure"
        }
      ]
    })";
  }

  std::unique_ptr<aember::config_manager::ConfigManager> manager_;
  std::string test_dir_;
};

}  // namespace aember_test::config_manager
