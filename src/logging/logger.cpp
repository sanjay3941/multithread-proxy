#include "logging/logger.hpp"

#include <chrono>
#include <ctime>
#include <cstdlib>
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
    const char* configured = std::getenv("PROXY_DETAILED_LOGS");
    const bool detailed = configured && std::string(configured) == "1";
    std::string safe = message;
    if (!detailed) {
        for (const std::string field : {"CLIENT=", "TARGET=", "PATH="}) {
            std::size_t start = 0;
            while ((start = safe.find(field, start)) != std::string::npos) {
                const auto end = safe.find(' ', start);
                safe.replace(start, end == std::string::npos ? safe.size() - start : end - start,
                             field + "[redacted]");
                start += field.size() + 10;
            }
        }
    }
    if (safe.rfind("==========", 0) != 0) {
        for (char& ch : safe) if (static_cast<unsigned char>(ch) < 32) ch = '?';
    }
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
         << safe << '\n';

    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << line.str() << std::flush;
    if (detailed) {
        std::error_code error;
        if (std::filesystem::exists(logPath_, error) &&
            std::filesystem::file_size(logPath_, error) > 1024 * 1024) {
            std::filesystem::remove(logPath_ + ".1", error);
            std::filesystem::rename(logPath_, logPath_ + ".1", error);
        }
        std::ofstream file(logPath_, std::ios::app);
        if (file) file << line.str() << std::flush;
    }
}

} // namespace logging
