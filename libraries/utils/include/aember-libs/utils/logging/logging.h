#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace aember::utils {

class Logger {
 public:
  Logger(const std::string& class_name);

  // Logging functions
  void info(const std::string& message, const std::string& func = "");
  void warn(const std::string& message, const std::string& func = "");
  void error(const std::string& message, const std::string& func = "");
  void debug(const std::string& message, const std::string& func = "");

 private:
  std::shared_ptr<spdlog::logger> logger_;
  std::string class_name_;
};

}  // namespace aember::utils
