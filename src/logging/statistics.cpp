#include "logging/statistics.hpp"

#include <iomanip>
#include <sstream>

namespace logging {

ProxyStatistics& ProxyStatistics::instance() {
    static ProxyStatistics statistics;
    return statistics;
}

std::shared_ptr<ConnectionStats> ProxyStatistics::begin(const std::string& clientIp,
                                                         std::uint16_t clientPort) {
    auto connection = std::make_shared<ConnectionStats>();
    const auto id = nextId_.fetch_add(1);
    std::ostringstream connectionId;
    connectionId << "REQ-" << std::setfill('0') << std::setw(6) << id;
    connection->connectionId = connectionId.str();
    connection->clientIp = clientIp;
    connection->clientPort = clientPort;
    totalConnections_.fetch_add(1);
    activeConnections_.fetch_add(1);
    return connection;
}

void ProxyStatistics::finish(const std::shared_ptr<ConnectionStats>& connection,
                             bool successful) {
    if (connection->finished.exchange(true)) return;
    connection->endTime = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        connection->endTime - connection->startTime).count();
    totalDurationMicroseconds_.fetch_add(static_cast<std::uint64_t>(duration));
    totalBytesReceived_.fetch_add(connection->bytesReceived.load());
    totalBytesSent_.fetch_add(connection->bytesSent.load());
    if (connection->protocol == "HTTPS") httpsConnections_.fetch_add(1);
    else httpConnections_.fetch_add(1);
    if (successful) successfulConnections_.fetch_add(1);
    else failedConnections_.fetch_add(1);
    activeConnections_.fetch_sub(1);
}

std::string ProxyStatistics::summary() const {
    const auto total = totalConnections_.load();
    const auto average = total == 0 ? 0.0
        : static_cast<double>(totalDurationMicroseconds_.load()) / total / 1000000.0;
    std::ostringstream output;
    output << "========== PROXY STATISTICS ==========\n"
           << "Total Connections      : " << total << '\n'
           << "Active Connections     : " << activeConnections_.load() << '\n'
           << "HTTP Connections       : " << httpConnections_.load() << '\n'
           << "HTTPS Connections      : " << httpsConnections_.load() << "\n\n"
           << "Successful Connections : " << successfulConnections_.load() << '\n'
           << "Failed Connections     : " << failedConnections_.load() << "\n\n"
           << "Bytes Received         : " << totalBytesReceived_.load() << '\n'
           << "Bytes Sent             : " << totalBytesSent_.load() << "\n\n"
           << std::fixed << std::setprecision(2)
           << "Average Duration       : " << average << " s\n"
           << "========================================";
    return output.str();
}

} // namespace logging