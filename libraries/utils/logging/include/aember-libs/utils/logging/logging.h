/**
 * @file logging.h
 * @author Arian Ajdari
 * @brief Lightweight logging utilities for Aember components
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace aember::utils {

void enable_console_silence();

/**
 * @brief Initialize early logging to stdout.
 *
 * Should be called once during early initialization before file logging
 * is available.
 */
void init_early_logging();

/**
 * @brief Enable file logging after pivot_root.
 *
 * Must be called once the real root filesystem is mounted. Injects the
 * file sink into all existing loggers and stores it globally so that
 * any Logger constructed afterwards also receives it.
 *
 * @param path Path to the log file.
 */
void enable_file_logging(const std::string& path);

/**
 * @brief Lightweight per-component logger.
 *
 * Always inherits global sink configuration, including the file sink
 * if enable_file_logging() has already been called.
 * Provides convenient logging methods with format string support.
 */
class Logger {
 public:
  /**
   * @brief Construct a logger for a specific component.
   *
   * @param name Component name for logger identification.
   */
  explicit Logger(const std::string& name);

  /// Log a trace-level message
  template <typename... Args>
  void trace(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->trace(fmt, std::forward<Args>(args)...);
  }

  /// Log a debug-level message
  template <typename... Args>
  void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->debug(fmt, std::forward<Args>(args)...);
  }

  /// Log an info-level message
  template <typename... Args>
  void info(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->info(fmt, std::forward<Args>(args)...);
  }

  /// Log a warning-level message
  template <typename... Args>
  void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->warn(fmt, std::forward<Args>(args)...);
  }

  /// Log an error-level message
  template <typename... Args>
  void error(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->error(fmt, std::forward<Args>(args)...);
  }

  /// Log a critical-level message
  template <typename... Args>
  void critical(fmt::format_string<Args...> fmt, Args&&... args) {
    logger_->critical(fmt, std::forward<Args>(args)...);
  }

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace aember::utils
