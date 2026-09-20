#include "logging/logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace logging {
namespace {

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO: return "INFO";
    case LogLevel::WARN: return "WARN";
    case LogLevel::ERROR: return "ERROR";
    }
    return "INFO";
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : logPath_("logs/proxy.log") {
    std::error_code error;
    std::filesystem::create_directories("logs", error);
}

void Logger::log(LogLevel level, const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&time, &localTime);

    std::ostringstream line;
    line << '[' << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count() << "] ["
         << levelName(level) << "] [THREAD=" << std::this_thread::get_id() << "] "
         << message << '\n';

    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << line.str() << std::flush;
    std::ofstream file(logPath_, std::ios::app);
    if (file) file << line.str() << std::flush;
}

} // namespace logging