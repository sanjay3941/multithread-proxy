#include "logging/statistics.hpp"

#include "proxy/privacy.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>

extern char** environ;

namespace logging {
namespace {
// Isolate potentially slow NSS/DNS resolution from proxy workers. The helper
// receives a numeric socket peer address as an argument, never shell input.
std::string lookupName(const std::string& ip) {
    in_addr numeric{};
    if (::inet_pton(AF_INET, ip.c_str(), &numeric) != 1) return {};
    if ((ntohl(numeric.s_addr) >> 24) == 127) {
        char name[256]{};
        if (::gethostname(name, sizeof(name) - 1) == 0) return name;
        return {};
    }
    int output[2];
    if (::pipe2(output, O_CLOEXEC | O_NONBLOCK) != 0) return {};
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(output[0]); ::close(output[1]); return {};
    }
    bool valid = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0;
    valid = valid && ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    valid = valid && ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0;
    // Do not let the resolver helper keep proxy sockets alive after closure.
    valid = valid && ::posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0;
    char executable[] = "/usr/bin/getent", database[] = "hosts";
    char* args[] = {executable, database, const_cast<char*>(ip.c_str()), nullptr};
    pid_t child = -1;
    const int spawned = valid ? ::posix_spawn(&child, executable, &actions, nullptr, args, environ) : -1;
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    if (spawned != 0) { ::close(output[0]); return {}; }
    std::string result;
    bool exited = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd readable{output[0], POLLIN, 0};
        ::poll(&readable, 1, 50);
        char buffer[1024];
        const auto count = ::read(output[0], buffer, sizeof(buffer));
        if (count > 0 && result.size() < 4096) result.append(buffer, count);
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) { exited = true; break; }
        if (waited < 0 && errno != EINTR) break;
    }
    if (!exited) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    ::close(output[0]);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    std::istringstream row(result);
    std::string address, hostname;
    row >> address >> hostname;
    if (address != ip || hostname.empty() || hostname.size() > 253 || hostname == ip) return {};
    for (unsigned char c : hostname)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return {};
    return hostname;
}
}

ProxyStatistics::ProxyStatistics() : namesThread_([this] { resolveNames(); }) {}
ProxyStatistics::~ProxyStatistics() {
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        stopping_ = true;
    }
    namesReady_.notify_one();
    namesThread_.join();
}

void ProxyStatistics::resolveNames() {
    for (;;) {
        std::shared_ptr<DeviceStats> device;
        {
            std::unique_lock<std::mutex> lock(devicesMutex_);
            namesReady_.wait(lock, [this] { return stopping_ || !pendingNames_.empty(); });
            if (stopping_) return;
            device = pendingNames_.front(); pendingNames_.pop_front();
        }
        const auto name = lookupName(device->ip);
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            device->name = name;
            device->nameStatus = name.empty() ? "unavailable" : "resolved";
        }
    }
}

std::string ProxyStatistics::devicesJson() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    std::ostringstream out;
    out << "{\"untracked\":" << untracked_ << ",\"items\":[";
    bool first = true;
    for (const auto& item : devices_) {
        const auto& d = *item.second;
        if (d.total == 0) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"ip\":" << proxy::Privacy::quote(d.ip)
            << ",\"name\":" << proxy::Privacy::quote(d.name)
            << ",\"nameStatus\":" << proxy::Privacy::quote(d.nameStatus)
            << ",\"total\":" << d.total << ",\"active\":" << d.active
            << ",\"successful\":" << d.successful << ",\"failed\":" << d.failed
            << ",\"blocked\":" << d.blocked << ",\"ads\":" << d.ads
            << ",\"trackers\":" << d.trackers << ",\"custom\":" << d.custom
            << ",\"received\":" << d.received << ",\"sent\":" << d.sent
            << ",\"lastSeenSeconds\":" << std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - d.seen).count() << '}';
    }
    out << "]}";
    return out.str();
}

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
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        auto found = devices_.find(clientIp);
        if (found == devices_.end() && devices_.size() < 256) {
            auto device = std::make_shared<DeviceStats>();
            device->ip = clientIp;
            found = devices_.emplace(clientIp, device).first;
            pendingNames_.push_back(device);
            namesReady_.notify_one();
        }
        if (found != devices_.end()) {
            connection->device = found->second;
            ++connection->device->total;
            ++connection->device->active;
        }
    }
    return connection;
}

void ProxyStatistics::finish(const std::shared_ptr<ConnectionStats>& connection,
                             bool successful) {
    if (connection->finished.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        if (auto device = connection->device) {
            --device->active;
            if (connection->dashboard) --device->total;
            else {
                device->seen = std::chrono::steady_clock::now();
                device->received += connection->bytesReceived.load();
                device->sent += connection->bytesSent.load();
                if (connection->blocked) {
                    ++device->blocked;
                    if (connection->blockedCategory == "ads") ++device->ads;
                    else if (connection->blockedCategory == "trackers") ++device->trackers;
                    else if (connection->blockedCategory == "custom") ++device->custom;
                } else if (successful) ++device->successful;
                else ++device->failed;
            }
        } else if (!connection->dashboard) ++untracked_;
    }
    if (connection->dashboard) {
        totalConnections_.fetch_sub(1);
        activeConnections_.fetch_sub(1);
        return;
    }
    connection->endTime = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        connection->endTime - connection->startTime).count();
    totalDurationMicroseconds_.fetch_add(static_cast<std::uint64_t>(duration));
    totalBytesReceived_.fetch_add(connection->bytesReceived.load());
    totalBytesSent_.fetch_add(connection->bytesSent.load());
    if (connection->protocol == "HTTPS") httpsConnections_.fetch_add(1);
    else httpConnections_.fetch_add(1);
    if (connection->blocked) blockedConnections_.fetch_add(1);
    else if (successful) successfulConnections_.fetch_add(1);
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
           << "Blocked Connections    : " << blockedConnections_.load() << '\n'
           << "Bytes Received         : " << totalBytesReceived_.load() << '\n'
           << "Bytes Sent             : " << totalBytesSent_.load() << "\n\n"
           << std::fixed << std::setprecision(2)
           << "Average Duration       : " << average << " s\n"
           << "========================================";
    return output.str();
}

std::string ProxyStatistics::json() const {
    std::ostringstream out;
    out << "{\"total\":" << totalConnections_.load()
        << ",\"active\":" << activeConnections_.load()
        << ",\"successful\":" << successfulConnections_.load()
        << ",\"failed\":" << failedConnections_.load()
        << ",\"blocked\":" << blockedConnections_.load()
        << ",\"http\":" << httpConnections_.load()
        << ",\"https\":" << httpsConnections_.load() << '}';
    return out.str();
}

} // namespace logging
