/**
 * @file logging.h
 * @author Arian Ajdari
 * @brief Logging for Aember components: one spdlog logger for the whole
 *        process.
 *
 *        There is exactly one spdlog::logger (the spdlog default logger).
 *        A Logger is only a component name: it formats "[<name>] <message>"
 *        and hands it to the global logger. Its single sink is a thread-safe
 *        dist_sink fanning out to the console and, once enabled, the file
 *        sink, so sinks can be added or muted while other threads log.
 *
 * @version 0.2
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace aember::utils::logging {

/// The process-wide spdlog logger, created on first use with a console sink.
spdlog::logger& global_logger();

void logging_level_debug();

void logging_level_info();

/// Remove the console sink for good (a process that only logs to its file).
void enable_console_silence();

/**
 * @brief Mute or unmute log output on the console (stdout) without removing
 *        the sink; the file log is unaffected. Used while a debug shell owns
 *        the console.
 */
void set_console_logging(bool enabled);

/// False while console output is muted by set_console_logging(false).
[[nodiscard]] bool console_logging_enabled();

/**
 * @brief Also log to a file (rotating, 1 MiB x 3). Later calls are ignored.
 *        A file that can't be opened is reported and skipped, never thrown.
 *
 * @param path Path to the log file.
 */
void enable_file_logging(const std::filesystem::path& path);

/// Flush every sink.
void flush();

/**
 * @brief Per-component logging handle: just the component name.
 *
 * Cheap to construct and copy; all Loggers share the one global logger.
 */
class Logger {
 public:
  /**
   * @brief Construct a logger for a specific component.
   *
   * @param name Component name shown as "[name]" in every line.
   */
  explicit Logger(std::string name);

  const std::string& name() const { return name_; }

  /// Log a trace-level message
  template <typename... Args>
  void trace(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::trace, fmt, std::forward<Args>(args)...);
  }

  /// Log a debug-level message
  template <typename... Args>
  void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::debug, fmt, std::forward<Args>(args)...);
  }

  /// Log an info-level message
  template <typename... Args>
  void info(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::info, fmt, std::forward<Args>(args)...);
  }

  /// Log a warning-level message
  template <typename... Args>
  void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::warn, fmt, std::forward<Args>(args)...);
  }

  /// Log an error-level message
  template <typename... Args>
  void error(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::err, fmt, std::forward<Args>(args)...);
  }

  /// Log a critical-level message
  template <typename... Args>
  void critical(fmt::format_string<Args...> fmt, Args&&... args) {
    Log(spdlog::level::critical, fmt, std::forward<Args>(args)...);
  }

 private:
  template <typename... Args>
  void Log(spdlog::level::level_enum level, fmt::format_string<Args...> fmt,
           Args&&... args) {
    auto& logger = global_logger();
    if (!logger.should_log(level)) return;
    Write(level, fmt::format(fmt, std::forward<Args>(args)...));
  }

  /// Non-template part, so each call site only instantiates the formatting.
  void Write(spdlog::level::level_enum level, std::string_view message);

  std::string name_;
};

}  // namespace aember::utils::logging
