#pragma once

#include <mutex>
#include <string>

namespace logging {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance();
    void log(LogLevel level, const std::string& message);

private:
    Logger();
    std::mutex mutex_;
    std::string logPath_;
};

} // namespace logging