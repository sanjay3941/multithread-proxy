#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace logging {

struct ConnectionStats {
    std::string connectionId;
    std::string clientIp;
    std::uint16_t clientPort = 0;
    std::string targetHost;
    std::string targetPort;
    std::string protocol = "HTTP";
    std::string method;
    std::string threadId;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point endTime{};
    std::atomic<std::uint64_t> bytesReceived{0};
    std::atomic<std::uint64_t> bytesSent{0};
    int httpStatus = 0;
    std::string status = "RECEIVED";
    std::string errorCode;
    std::string errorMessage;
    mutable std::mutex metadataMutex;
    std::atomic_bool finished{false};
};

class ProxyStatistics {
public:
    static ProxyStatistics& instance();
    std::shared_ptr<ConnectionStats> begin(const std::string& clientIp,
                                           std::uint16_t clientPort);
    void finish(const std::shared_ptr<ConnectionStats>& connection, bool successful);
    std::string summary() const;

private:
    std::atomic<std::uint64_t> nextId_{1};
    std::atomic<std::uint64_t> totalConnections_{0};
    std::atomic<std::uint64_t> activeConnections_{0};
    std::atomic<std::uint64_t> httpConnections_{0};
    std::atomic<std::uint64_t> httpsConnections_{0};
    std::atomic<std::uint64_t> successfulConnections_{0};
    std::atomic<std::uint64_t> failedConnections_{0};
    std::atomic<std::uint64_t> totalBytesReceived_{0};
    std::atomic<std::uint64_t> totalBytesSent_{0};
    std::atomic<std::uint64_t> totalDurationMicroseconds_{0};
};

} // namespace logging