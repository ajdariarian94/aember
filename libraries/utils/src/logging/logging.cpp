/**
 * @file logging.cpp
 * @author Arian Ajdari
 * @brief Implementation of lightweight logging utilities for Aember components
 * @version 0.1
 * @date 2025-12-22
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/logging/logging.h>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <vector>

namespace aember::utils {

// Mutex to protect sink initialization
static std::mutex sink_mutex;

// Track if file logging has been enabled
static bool file_logging_enabled = false;

/**
 * @brief Global sink set inherited by all loggers.
 */
static std::vector<spdlog::sink_ptr> global_sinks;

// Default log pattern: timestamp, level, logger name, message
static constexpr const char* LOG_PATTERN =
    "[%Y-%m-%d %H:%M:%S] %^[%l]%$ [%n] %v";

/**
 * @brief Apply the default formatter to a sink.
 *
 * @param sink Sink to apply formatter to.
 */
static void apply_formatter(const spdlog::sink_ptr& sink) {
  sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN));
}

/**
 * @brief Initialize early logging to stdout.
 *
 * Should be called once during early init before file logging is available.
 * Creates a root logger and sets it as default.
 */
void init_early_logging() {
  std::lock_guard lock(sink_mutex);

  if (!global_sinks.empty()) return;

  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  apply_formatter(stdout_sink);

  global_sinks.push_back(stdout_sink);

  auto root_logger = std::make_shared<spdlog::logger>(
      "root", global_sinks.begin(), global_sinks.end());

  spdlog::set_default_logger(root_logger);
  spdlog::set_level(spdlog::level::info);
}

/**
 * @brief Enable file logging for all loggers.
 *
 * Must be called after the root filesystem is available.
 * Existing loggers will inherit the new file sink.
 *
 * @param log_file_path Path to the log file.
 */
void enable_file_logging(const std::string& log_file_path) {
  std::lock_guard lock(sink_mutex);

  if (file_logging_enabled) return;

  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      log_file_path, /*truncate=*/false);

  apply_formatter(file_sink);
  global_sinks.push_back(file_sink);

  // Apply updated sinks to all existing loggers
  spdlog::apply_all([&](const std::shared_ptr<spdlog::logger>& logger) {
    logger->sinks() = global_sinks;
  });

  file_logging_enabled = true;
}

/**
 * @brief Construct a component logger.
 *
 * Inherits all existing sinks from the default logger.
 *
 * @param name Component name for the logger.
 */
Logger::Logger(const std::string& name) {
  std::lock_guard lock(sink_mutex);

  logger_ = spdlog::get(name);
  if (logger_) return;

  // Clone default logger to inherit all sinks
  logger_ = spdlog::default_logger()->clone(name);
  spdlog::register_logger(logger_);
}

}  // namespace aember::utils
