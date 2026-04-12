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

namespace aember::utils::logging {

static std::mutex sink_mutex;
static bool early_initialized = false;
static bool file_logging_enabled = false;

// All loggers always share THESE sinks
static std::vector<spdlog::sink_ptr> global_sinks;

// SINGLE SOURCE OF TRUTH for log pattern
static constexpr const char* LOG_PATTERN =
    "[%Y-%m-%d %H:%M:%S] %^[%l]%$ [%n] %v";

static std::unique_ptr<spdlog::formatter> make_formatter() {
  return std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN);
}

void enable_console_silence() {
  std::lock_guard lock(sink_mutex);
  // Remove the stdout sink from every logger
  spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
    auto& sinks = logger->sinks();
    sinks.erase(
        std::remove_if(sinks.begin(),
                       sinks.end(),
                       [](const spdlog::sink_ptr& s) {
                         return std::dynamic_pointer_cast<
                                    spdlog::sinks::stdout_color_sink_mt>(s) !=
                                nullptr;
                       }),
        sinks.end());
  });
  // Also remove from global_sinks so new loggers don't get it either
  global_sinks.erase(
      std::remove_if(global_sinks.begin(),
                     global_sinks.end(),
                     [](const spdlog::sink_ptr& s) {
                       return std::dynamic_pointer_cast<
                                  spdlog::sinks::stdout_color_sink_mt>(s) !=
                              nullptr;
                     }),
      global_sinks.end());
}

void init_early_logging() {
  std::lock_guard lock(sink_mutex);
  if (early_initialized) return;
  early_initialized = true;

  spdlog::drop_all();    // drop registered loggers
  global_sinks.clear();  // NO spdlog::shutdown() here

  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  stdout_sink->set_formatter(make_formatter());
  global_sinks.push_back(stdout_sink);

  auto root_logger = std::make_shared<spdlog::logger>(
      "logging", global_sinks.begin(), global_sinks.end());
  spdlog::set_default_logger(root_logger);
  spdlog::set_level(spdlog::level::info);
}

void enable_file_logging(const std::filesystem::path& path) {
  std::lock_guard lock(sink_mutex);
  if (file_logging_enabled) return;
  file_logging_enabled = true;

  // Ensure directory exists
  std::filesystem::create_directories(path.parent_path());

  // Build the file sink with the same pattern as console
  auto file_sink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), false);
  file_sink->set_formatter(make_formatter());
  file_sink->set_level(spdlog::level::trace);

  // Store it globally so Logger() constructors called later also pick it up
  global_sinks.push_back(file_sink);

  // Backfill every already-registered logger
  spdlog::apply_all([&](std::shared_ptr<spdlog::logger> logger) {
    logger->sinks().push_back(file_sink);
    logger->flush_on(spdlog::level::trace);
  });

  // Flush all loggers to file every second regardless of level
  spdlog::flush_every(std::chrono::seconds(1));

  spdlog::default_logger()->info("{} file logging initialized: {}",
                                 path.filename().c_str(),
                                 path.string());
}

Logger::Logger(const std::string& name) {
  std::lock_guard lock(sink_mutex);

  // Ensure early logging is bootstrapped even if caller forgot
  if (!early_initialized) {
    // Inline the init here without re-locking (we already hold the lock)
    early_initialized = true;

    spdlog::drop_all();
    global_sinks.clear();

    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    stdout_sink->set_formatter(make_formatter());
    global_sinks.push_back(stdout_sink);

    auto root_logger = std::make_shared<spdlog::logger>(
        "logging", global_sinks.begin(), global_sinks.end());
    spdlog::set_default_logger(root_logger);
    spdlog::set_level(spdlog::level::info);
  }

  if (auto existing = spdlog::get(name)) {
    logger_ = existing;
    return;
  }

  logger_ = std::make_shared<spdlog::logger>(
      name, global_sinks.begin(), global_sinks.end());
  logger_->set_level(spdlog::default_logger()->level());
  logger_->flush_on(spdlog::level::trace);
  spdlog::register_logger(logger_);
}

}  // namespace aember::utils::logging
