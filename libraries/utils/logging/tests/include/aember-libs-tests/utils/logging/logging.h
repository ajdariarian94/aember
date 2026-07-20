#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <string>

#include <gtest/gtest.h>

namespace aember_test::utils {

class LoggingTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  // Helper to create loggers
  aember::utils::Logger make_logger(const std::string& name);

  // Test log file path
  std::string log_file_path_;
};

}  // namespace aember_test::utils
