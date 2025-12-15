#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace aember::utils {

/**
 * Initialize early logging (stdout only).
 * Call once during early init.
 */
void init_early_logging();

/**
 * Enable file logging after pivot_root.
 * Safe to call once real root is mounted.
 */
void enable_file_logging(const std::string& log_file_path);

/**
 * Lightweight per-component logger.
 */
class Logger {
 public:
  explicit Logger(const std::string& name);

  template <typename... Args>
  void trace(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->trace(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->debug(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void info(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->info(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->warn(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void error(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->error(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void critical(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->critical(fmt, std::forward<Args>(args)...);
  }

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace aember::utils
