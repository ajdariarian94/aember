#include <aember-libs/utils/logging/logging.h>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>

namespace aember::utils {

// Ensure pattern is set only once globally
static std::once_flag spdlog_pattern_once;

Logger::Logger(const std::string& class_name)
    : class_name_(class_name)
{
    // Create or fetch logger for this class
    logger_ = spdlog::get(class_name_);
    if (!logger_) {
        logger_ = spdlog::stdout_color_mt(class_name_);
    }

    // Set pattern only once
    std::call_once(spdlog_pattern_once, []() {
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] %^[%l]%$ [%n] %v");
    });
}

void Logger::info(const std::string& message, const std::string& func) {
    if (func.empty())
        logger_->info("{}", message);
    else
        logger_->info("[{}] {}", func, message);
}

void Logger::warn(const std::string& message, const std::string& func) {
    if (func.empty())
        logger_->warn("{}", message);
    else
        logger_->warn("[{}] {}", func, message);
}

void Logger::error(const std::string& message, const std::string& func) {
    if (func.empty())
        logger_->error("{}", message);
    else
        logger_->error("[{}] {}", func, message);
}

void Logger::debug(const std::string& message, const std::string& func) {
    if (func.empty())
        logger_->debug("{}", message);
    else
        logger_->debug("[{}] {}", func, message);
}

}