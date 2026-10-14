#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace logging {

// All fields are protected by ProxyStatistics::devicesMutex_.
struct DeviceStats {
    std::string ip, name, nameStatus = "pending";
    std::uint64_t total = 0, active = 0, successful = 0, failed = 0, blocked = 0;
    std::uint64_t ads = 0, trackers = 0, custom = 0, received = 0, sent = 0;
    std::chrono::steady_clock::time_point seen = std::chrono::steady_clock::now();
};

struct ConnectionStats {
    std::shared_ptr<DeviceStats> device;
    std::string blockedCategory;
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
    bool blocked = false;
    bool dashboard = false;
    std::string status = "RECEIVED";
    std::string errorCode;
    std::string errorMessage;
    mutable std::mutex metadataMutex;
    std::atomic_bool finished{false};
};

class ProxyStatistics {
public:
    static ProxyStatistics& instance();
    ~ProxyStatistics();
    std::shared_ptr<ConnectionStats> begin(const std::string& clientIp,
                                           std::uint16_t clientPort);
    void finish(const std::shared_ptr<ConnectionStats>& connection, bool successful);
    std::string summary() const;
    std::string json() const;
    std::string devicesJson() const;

private:
    ProxyStatistics();
    void resolveNames();
    mutable std::mutex devicesMutex_;
    std::condition_variable namesReady_;
    std::map<std::string, std::shared_ptr<DeviceStats>> devices_;
    std::deque<std::shared_ptr<DeviceStats>> pendingNames_;
    bool stopping_ = false;
    std::thread namesThread_;
    std::uint64_t untracked_ = 0;
    std::atomic<std::uint64_t> nextId_{1};
    std::atomic<std::uint64_t> totalConnections_{0};
    std::atomic<std::uint64_t> activeConnections_{0};
    std::atomic<std::uint64_t> httpConnections_{0};
    std::atomic<std::uint64_t> httpsConnections_{0};
    std::atomic<std::uint64_t> successfulConnections_{0};
    std::atomic<std::uint64_t> failedConnections_{0};
    std::atomic<std::uint64_t> blockedConnections_{0};
    std::atomic<std::uint64_t> totalBytesReceived_{0};
    std::atomic<std::uint64_t> totalBytesSent_{0};
    std::atomic<std::uint64_t> totalDurationMicroseconds_{0};
};

} // namespace logging
