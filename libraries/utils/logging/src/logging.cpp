/**
 * @file logging.cpp
 * @author Arian Ajdari
 * @brief One spdlog logger for the whole process.
 * @version 0.2
 * @date 2026-11-17
 *
 * @copyright Copyright (c) 2025, Aember, All rights reserved.
 */

#include <aember-libs/utils/logging/logging.h>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <memory>
#include <mutex>

namespace aember::utils::logging {

namespace {

constexpr std::size_t kLogFileMaxBytes = 1024 * 1024;
constexpr std::size_t kLogFileMaxFiles = 3;
// The component name is part of the message ("[name] text"), so the pattern
// has no %n: there is only one logger.
constexpr const char* kLogPattern = "[%Y-%m-%d %H:%M:%S] [%l] %v";

std::unique_ptr<spdlog::formatter> MakeFormatter() {
  return std::make_unique<spdlog::pattern_formatter>(kLogPattern);
}

/// The single logger, its fan-out sink and the sinks we toggle.
struct State {
  std::shared_ptr<spdlog::sinks::dist_sink_mt> dist;
  std::shared_ptr<spdlog::sinks::stdout_sink_mt> console;
  bool console_attached = false;
  bool file_attached = false;
  std::shared_ptr<spdlog::logger> logger;
  std::mutex mutex;  // guards console_attached / file_attached
};

State& GetState() {
  // Function-local static: created on first use, thread-safe.
  static State* state = [] {
    auto* s = new State;  // never destroyed: threads may log during exit
    s->dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
    s->console = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    s->console->set_formatter(MakeFormatter());
    s->dist->add_sink(s->console);
    s->console_attached = true;

    s->logger = std::make_shared<spdlog::logger>("aember", s->dist);
    s->logger->set_level(spdlog::level::info);
    // Write through on every info line: PID 1 can lose power any time.
    s->logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(s->logger);
    return s;
  }();
  return *state;
}

}  // namespace

spdlog::logger& global_logger() {
  return *GetState().logger;
}

void logging_level_debug() {
  global_logger().set_level(spdlog::level::debug);
}

void logging_level_info() {
  global_logger().set_level(spdlog::level::info);
}

void init_early_logging() {
  GetState();
}

void flush() {
  global_logger().flush();
}

void enable_console_silence() {
  auto& s = GetState();
  std::lock_guard lock{s.mutex};
  if (!s.console_attached) return;
  s.logger->flush();
  s.dist->remove_sink(s.console);
  s.console_attached = false;
}

void set_console_logging(bool enabled) {
  auto& s = GetState();
  s.logger->flush();
  s.console->set_level(enabled ? spdlog::level::trace : spdlog::level::off);
}

void enable_file_logging(const std::filesystem::path& path) {
  auto& s = GetState();
  {
    std::lock_guard lock{s.mutex};
    if (s.file_attached) return;

    // Rotate at 1 MiB, keep 3 old files: PID 1 runs for the device's
    // lifetime and must not fill the disk. A log file is optional, so a
    // read-only or missing /var/log only costs the file, never init.
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file;
    try {
      std::filesystem::create_directories(path.parent_path());
      file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          path.string(), kLogFileMaxBytes, kLogFileMaxFiles);
    } catch (const std::exception& e) {
      s.logger->error(
          "[logging] file logging to {} disabled: {}", path.string(), e.what());
      return;
    }
    file->set_formatter(MakeFormatter());
    s.dist->add_sink(file);
    s.file_attached = true;
  }
  s.logger->info("[logging] file logging to {}", path.string());
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger::Logger(std::string name) : name_(std::move(name)) {
  GetState();
}

void Logger::Write(spdlog::level::level_enum level, std::string_view message) {
  global_logger().log(level, "[{}] {}", name_, message);
}

}  // namespace aember::utils::logging
