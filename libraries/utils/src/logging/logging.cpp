/**
 * @file logging.cpp
 * @brief Logging utilities for Aember
 */

#include <aember-libs/utils/logging/logging.h>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <vector>

namespace aember::utils {

static std::mutex sink_mutex;
static bool initialized = false;
static bool file_logging_enabled = false;

// All loggers always share THESE sinks
static std::vector<spdlog::sink_ptr> global_sinks;

// SINGLE SOURCE OF TRUTH
static constexpr const char* LOG_PATTERN =
    "[%Y-%m-%d %H:%M:%S] %^[%l]%$ [%n] %v";

static std::unique_ptr<spdlog::formatter> make_formatter() {
  return std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN);
}

void init_early_logging() {
  std::lock_guard lock(sink_mutex);
  if (initialized) return;
  initialized = true;

  // 🔥 Hard reset spdlog state (prevents stale sinks/formatters)
  spdlog::shutdown();
  spdlog::drop_all();
  global_sinks.clear();

  // Console sink (colored)
  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  stdout_sink->set_formatter(make_formatter());
  global_sinks.push_back(stdout_sink);

  // Root logger
  auto root_logger = std::make_shared<spdlog::logger>(
      "root", global_sinks.begin(), global_sinks.end());

  spdlog::set_default_logger(root_logger);
  spdlog::set_level(spdlog::level::info);
}

void enable_file_logging(const std::string& log_file_path) {
  std::lock_guard lock(sink_mutex);
  if (file_logging_enabled) return;

  auto file_sink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, false);

  file_sink->set_formatter(make_formatter());
  global_sinks.push_back(file_sink);

  // Reattach sinks to all existing loggers
  spdlog::apply_all([&](const std::shared_ptr<spdlog::logger>& logger) {
    logger->sinks() = global_sinks;
  });

  file_logging_enabled = true;
}

Logger::Logger(const std::string& name) {
  std::lock_guard lock(sink_mutex);

  if (auto existing = spdlog::get(name)) {
    logger_ = existing;
    return;
  }

  // Clone root logger → inherits sinks + formatter
  logger_ = spdlog::default_logger()->clone(name);
  spdlog::register_logger(logger_);
}

}  // namespace aember::utils
