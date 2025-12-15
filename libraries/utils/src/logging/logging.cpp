#include <aember-libs/utils/logging/logging.h>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>

namespace aember::utils {

static std::mutex sink_mutex;
static bool file_logging_enabled = false;

static constexpr const char* LOG_PATTERN =
    "[%Y-%m-%d %H:%M:%S] %^[%l]%$ [%n] %v";

void init_early_logging() {
  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  stdout_sink->set_formatter(
      std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN));

  auto root_logger = std::make_shared<spdlog::logger>("root", stdout_sink);

  spdlog::set_default_logger(root_logger);
  spdlog::set_level(spdlog::level::info);

  // Fix already-created loggers (init-order safe)
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
    for (auto& sink : logger->sinks()) {
      sink->set_formatter(
          std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN));
    }
  });
}

void enable_file_logging(const std::string& log_file_path) {
  std::lock_guard lock(sink_mutex);

  if (file_logging_enabled) return;

  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      log_file_path, /*truncate=*/false);

  file_sink->set_formatter(
      std::make_unique<spdlog::pattern_formatter>(LOG_PATTERN));

  spdlog::apply_all([&](const std::shared_ptr<spdlog::logger>& logger) {
    logger->sinks().push_back(file_sink);
  });

  file_logging_enabled = true;
}

Logger::Logger(const std::string& name) {
  logger_ = spdlog::get(name);
  if (!logger_) {
    logger_ = spdlog::default_logger()->clone(name);
    spdlog::register_logger(logger_);
  }
}

}  // namespace aember::utils
